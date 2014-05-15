// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/store.hpp"

#include <functional>

#include "arch/runtime/coroutines.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/operations.hpp"
#include "btree/secondary_operations.hpp"
#include "btree/slice.hpp"
#include "buffer_cache/alt/alt.hpp"
#include "buffer_cache/alt/cache_balancer.hpp"
#include "concurrency/wait_any.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/disk_backed_queue.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"
#include "serializer/config.hpp"
#include "stl_utils.hpp"
#include "btree/internal_node.hpp"

// Some of this implementation is in store.cc and some in btree_store.cc for no
// particularly good reason.  Historically it turned out that way, and for now
// there's not enough refactoring urgency to combine them into one.
//
// (Really, some of the implementation is also in rdb_protocol/btree.cc.)

sindex_not_ready_exc_t::sindex_not_ready_exc_t(
        std::string sindex_name,
        const secondary_index_t &sindex,
        const std::string &table_name) {
    if (sindex.being_deleted) {
        rassert(false, "A query tried to access index `%s` on table `%s` which is "
                       "currently being deleted. Queries should not be able to "
                       "access such indexes in the first place, so this is a bug.",
                sindex_name.c_str(),
                table_name.c_str());
        info = strprintf("Index `%s` on table `%s` "
                         "was accessed while it was being deleted.",
                         sindex_name.c_str(),
                         table_name.c_str());
    } else {
        rassert(!sindex.post_construction_complete);
        info = strprintf("Index `%s` on table `%s` "
                         "was accessed before its construction was finished.",
                         sindex_name.c_str(),
                         table_name.c_str());
    }
}

const char* sindex_not_ready_exc_t::what() const throw() {
    return info.c_str();
}

sindex_not_ready_exc_t::~sindex_not_ready_exc_t() throw() { }

store_t::store_t(serializer_t *serializer,
                 cache_balancer_t *balancer,
                 const std::string &perfmon_name,
                 bool create,
                 perfmon_collection_t *parent_perfmon_collection,
                 rdb_context_t *_ctx,
                 io_backender_t *io_backender,
                 const base_path_t &base_path)
    : store_view_t(region_t::universe()),
      perfmon_collection(),
      io_backender_(io_backender), base_path_(base_path),
      perfmon_collection_membership(parent_perfmon_collection, &perfmon_collection, perfmon_name),
      ctx(_ctx)
{
    cache.init(new cache_t(serializer, balancer, &perfmon_collection));
    general_cache_conn.init(new cache_conn_t(cache.get()));

    if (create) {
        vector_stream_t key;
        write_message_t wm;
        region_t kr = region_t::universe();
        serialize(&wm, kr);
        key.reserve(wm.size());
        int res = send_write_message(&key, &wm);
        guarantee(!res);

        txn_t txn(general_cache_conn.get(), write_durability_t::HARD,
                  repli_timestamp_t::distant_past, 1);
        buf_lock_t superblock(&txn, SUPERBLOCK_ID, alt_create_t::create);
        btree_slice_t::init_superblock(&superblock, key.vector(), std::vector<char>());
    }

    btree.init(new btree_slice_t(cache.get(), &perfmon_collection, "primary"));

    // Initialize sindex slices
    {
        // Since this is the btree constructor, nothing else should be locking these
        // things yet, so this should work fairly quickly and does not need a real
        // interruptor.
        cond_t dummy_interruptor;
        read_token_pair_t token_pair;
        store_view_t::new_read_token_pair(&token_pair);

        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_read(&token_pair.main_read_token, &txn,
                                    &superblock, &dummy_interruptor, false);

        buf_lock_t sindex_block
            = acquire_sindex_block_for_read(superblock->expose_buf(),
                                            superblock->get_sindex_block_id());

        std::map<std::string, secondary_index_t> sindexes;
        get_secondary_indexes(&sindex_block, &sindexes);

        for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
            secondary_index_slices.insert(it->second.id,
                                          new btree_slice_t(cache.get(),
                                                            &perfmon_collection,
                                                            it->first));
        }
    }

    help_construct_bring_sindexes_up_to_date();
}

store_t::~store_t() {
    assert_thread();
}

void store_t::read(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const read_t &read,
        read_response_t *response,
        UNUSED order_token_t order_token,  // TODO
        read_token_pair_t *token_pair,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;

    acquire_superblock_for_read(&token_pair->main_read_token, &txn, &superblock,
                                interruptor,
                                read.use_snapshot());

    DEBUG_ONLY(check_metainfo(DEBUG_ONLY(metainfo_checker, ) superblock.get());)

    protocol_read(read, response, superblock.get(), interruptor);
}

void store_t::write(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const metainfo_t& new_metainfo,
        const write_t &write,
        write_response_t *response,
        const write_durability_t durability,
        transition_timestamp_t timestamp,
        UNUSED order_token_t order_token,  // TODO
        write_token_pair_t *token_pair,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> real_superblock;
    const int expected_change_count = 2; // FIXME: this is incorrect, but will do for now
    acquire_superblock_for_write(timestamp.to_repli_timestamp(),
                                 expected_change_count, durability, token_pair,
                                 &txn, &real_superblock, interruptor);

    check_and_update_metainfo(DEBUG_ONLY(metainfo_checker, ) new_metainfo,
                              real_superblock.get());
    scoped_ptr_t<superblock_t> superblock(real_superblock.release());
    protocol_write(write, response, timestamp, &superblock, interruptor);
}

// TODO: Figure out wtf does the backfill filtering, figure out wtf constricts delete range operations to hit only a certain hash-interval, figure out what filters keys.
bool store_t::send_backfill(
        const region_map_t<state_timestamp_t> &start_point,
        send_backfill_callback_t *send_backfill_cb,
        traversal_progress_combiner_t *progress,
        read_token_pair_t *token_pair,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_backfill(&token_pair->main_read_token, &txn, &superblock, interruptor);

    buf_lock_t sindex_block
        = acquire_sindex_block_for_read(superblock->expose_buf(),
                                        superblock->get_sindex_block_id());

    region_map_t<binary_blob_t> unmasked_metainfo;
    get_metainfo_internal(superblock->get(), &unmasked_metainfo);
    region_map_t<binary_blob_t> metainfo = unmasked_metainfo.mask(start_point.get_domain());
    if (send_backfill_cb->should_backfill(metainfo)) {
        protocol_send_backfill(start_point, send_backfill_cb, superblock.get(), &sindex_block, progress, interruptor);
        return true;
    }
    return false;
}

void store_t::receive_backfill(
        const backfill_chunk_t &chunk,
        write_token_pair_t *token_pair,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> real_superblock;
    const int expected_change_count = 1; // TODO: this is not correct

    // We use HARD durability because we want backfilling to be throttled if we
    // receive data faster than it can be written to disk. Otherwise we might
    // exhaust the cache's dirty page limit and bring down the whole table.
    // Other than that, the hard durability guarantee is not actually
    // needed here.
    acquire_superblock_for_write(chunk.get_btree_repli_timestamp(),
                                 expected_change_count,
                                 write_durability_t::HARD,
                                 token_pair,
                                 &txn,
                                 &real_superblock,
                                 interruptor);

    scoped_ptr_t<superblock_t> superblock(real_superblock.release());
    protocol_receive_backfill(std::move(superblock),
                              interruptor,
                              chunk);
}

void store_t::reset_data(
        const region_t &subregion,
        const metainfo_t &new_metainfo,
        write_token_pair_t *token_pair,
        const write_durability_t durability,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;

    // We're passing 2 for the expected_change_count based on the
    // reasoning that we're probably going to touch a leaf-node-sized
    // range of keys and that it won't be aligned right on a leaf node
    // boundary.
    // TOnDO that's not reasonable; reset_data() is sometimes used to wipe out
    // entire databases.
    const int expected_change_count = 2;
    acquire_superblock_for_write(repli_timestamp_t::invalid,
                                 expected_change_count,
                                 durability,
                                 token_pair,
                                 &txn,
                                 &superblock,
                                 interruptor);

    region_map_t<binary_blob_t> old_metainfo;
    get_metainfo_internal(superblock->get(), &old_metainfo);
    update_metainfo(old_metainfo, new_metainfo, superblock.get());

    protocol_reset_data(subregion,
                        superblock.get(),
                        interruptor);
}

scoped_ptr_t<new_mutex_in_line_t> store_t::get_in_line_for_sindex_queue(
        buf_lock_t *sindex_block) {
    assert_thread();
    // The line for the sindex queue is there to guarantee that we push things to
    // the sindex queue(s) in the same order in which we held the sindex block.
    // It allows us to release the sindex block before starting to push data to the
    // sindex queues without sacrificing that guarantee.
    // Pushing data to those queues can take a while, and we wouldn't want to block
    // other transactions while we are doing that.

    guarantee(!sindex_block->empty());
    if (sindex_block->access() == access_t::write) {
        sindex_block->write_acq_signal()->wait();
    } else {
        // `bring_sindexes_up_to_date()` calls `lock_sindex_queue()` with only a read
        // acquisition. It is ok in that context, and we have to handle it here.
        sindex_block->read_acq_signal()->wait();
    }
    return scoped_ptr_t<new_mutex_in_line_t>(
            new new_mutex_in_line_t(&sindex_queue_mutex));
}

void store_t::register_sindex_queue(
            internal_disk_backed_queue_t *disk_backed_queue,
            const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (std::vector<internal_disk_backed_queue_t *>::iterator it = sindex_queues.begin();
            it != sindex_queues.end(); ++it) {
        guarantee(*it != disk_backed_queue);
    }
    sindex_queues.push_back(disk_backed_queue);
}

void store_t::deregister_sindex_queue(
            internal_disk_backed_queue_t *disk_backed_queue,
            const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (std::vector<internal_disk_backed_queue_t *>::iterator it = sindex_queues.begin();
            it != sindex_queues.end(); ++it) {
        if (*it == disk_backed_queue) {
            sindex_queues.erase(it);
            return;
        }
    }
}

void store_t::emergency_deregister_sindex_queue(
    internal_disk_backed_queue_t *disk_backed_queue) {
    assert_thread();
    drainer.assert_draining();
    new_mutex_in_line_t acq(&sindex_queue_mutex);
    acq.acq_signal()->wait_lazily_unordered();

    deregister_sindex_queue(disk_backed_queue, &acq);
}

void store_t::sindex_queue_push(const write_message_t &value,
                                const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (auto it = sindex_queues.begin(); it != sindex_queues.end(); ++it) {
        (*it)->push(value);
    }
}

void store_t::sindex_queue_push(
        const scoped_array_t<write_message_t> &values,
        const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (auto it = sindex_queues.begin(); it != sindex_queues.end(); ++it) {
        (*it)->push(values);
    }
}

void store_t::add_progress_tracker(
        map_insertion_sentry_t<uuid_u, const parallel_traversal_progress_t *> *sentry,
        uuid_u id, const parallel_traversal_progress_t *p) {
    assert_thread();
    sentry->reset(&progress_trackers, id, p);
}

progress_completion_fraction_t store_t::get_progress(uuid_u id) {
    if (!std_contains(progress_trackers, id)) {
        return progress_completion_fraction_t();
    } else {
        return progress_trackers[id]->guess_completion();
    }
}

// KSI: If we're going to have these functions at all, we could just pass the
// real_superblock_t directly.
buf_lock_t store_t::acquire_sindex_block_for_read(
        buf_parent_t parent,
        block_id_t sindex_block_id) {
    return buf_lock_t(parent, sindex_block_id, access_t::read);
}

buf_lock_t store_t::acquire_sindex_block_for_write(
        buf_parent_t parent,
        block_id_t sindex_block_id) {
    return buf_lock_t(parent, sindex_block_id, access_t::write);
}

bool store_t::add_sindex(
        const std::string &id,
        const secondary_index_t::opaque_definition_t &definition,
        buf_lock_t *sindex_block) {
    secondary_index_t sindex;
    if (::get_secondary_index(sindex_block, id, &sindex)) {
        return false; // sindex was already created
    } else {
        {
            buf_lock_t sindex_superblock(sindex_block, alt_create_t::create);
            sindex.superblock = sindex_superblock.block_id();
            sindex.opaque_definition = definition;

            /* Notice we're passing in empty strings for metainfo. The metainfo in
             * the sindexes isn't used for anything but this could perhaps be
             * something that would give a better error if someone did try to use
             * it... on the other hand this code isn't exactly idiot proof even
             * with that. */
            btree_slice_t::init_superblock(&sindex_superblock,
                                           std::vector<char>(), std::vector<char>());
        }

        secondary_index_slices.insert(
            sindex.id, new btree_slice_t(cache.get(), &perfmon_collection, id));

        sindex.post_construction_complete = false;

        ::set_secondary_index(sindex_block, id, sindex);
        return true;
    }
}

std::string compute_sindex_deletion_id(uuid_u sindex_uuid) {
    // Add a '\0' at the end to make absolutely sure it can't collide with
    // regular sindex names.
    return "_DEL_" + uuid_to_str(sindex_uuid) + "\0";
}

class clear_sindex_traversal_cb_t
        : public depth_first_traversal_callback_t {
public:
    clear_sindex_traversal_cb_t() {
        collected_keys.reserve(CHUNK_SIZE);
    }
    done_traversing_t handle_pair(scoped_key_value_t &&keyvalue) {
        collected_keys.push_back(store_key_t(keyvalue.key()));
        if (collected_keys.size() >= CHUNK_SIZE) {
            return done_traversing_t::YES;
        } else {
            return done_traversing_t::NO;
        }
    }
    const std::vector<store_key_t> &get_keys() const {
        return collected_keys;
    }
    static const size_t CHUNK_SIZE = 32;
private:
    std::vector<store_key_t> collected_keys;
};

void store_t::clear_sindex(
        secondary_index_t sindex,
        value_sizer_t *sizer,
        const deletion_context_t *deletion_context,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {

    /* Delete one piece of the secondary index at a time */
    for (bool reached_end = false; !reached_end;)
    {
        /* Start a transaction (1). */
        write_token_pair_t token_pair;
        store_view_t::new_write_token_pair(&token_pair);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_write(
            repli_timestamp_t::distant_past,
            clear_sindex_traversal_cb_t::CHUNK_SIZE /* Not really the right value,
                                                     * since many keys will share a
                                                     * leaf node */,
            write_durability_t::SOFT,
            &token_pair,
            &txn,
            &superblock,
            interruptor);

        /* Get the sindex block (1). */
        buf_lock_t sindex_block = acquire_sindex_block_for_write(
                superblock->expose_buf(),
                superblock->get_sindex_block_id());
        superblock->release();

        /* Clear part of the index data */
        buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                          sindex.superblock, access_t::write);
        sindex_block.reset_buf_lock();
        scoped_ptr_t<superblock_t> sindex_superblock;
        sindex_superblock.init(
            new real_superblock_t(std::move(sindex_superblock_lock)));

        /* 1. Collect a bunch of keys to delete */
        clear_sindex_traversal_cb_t traversal_cb;
        reached_end = btree_depth_first_traversal(sindex_superblock.get(),
                                 key_range_t::universe(),
                                 &traversal_cb,
                                 direction_t::FORWARD,
                                 release_superblock_t::KEEP);

        /* 2. Actually delete them */
        const std::vector<store_key_t> &keys = traversal_cb.get_keys();
        for (size_t i = 0; i < keys.size(); ++i) {
            promise_t<superblock_t *> superblock_promise;
            {
                keyvalue_location_t kv_location;
                find_keyvalue_location_for_write(sizer, sindex_superblock.release(),
                        keys[i].btree_key(), deletion_context->balancing_detacher(),
                        &kv_location, NULL /* stats */, NULL /* trace */,
                        &superblock_promise);

                deletion_context->in_tree_deleter()->delete_value(
                    buf_parent_t(&kv_location.buf), kv_location.value.get());
                kv_location.value.reset();
                if (kv_location.there_originally_was_value) {
                    buf_write_t write(&kv_location.buf);
                    auto leaf_node = static_cast<leaf_node_t *>(write.get_data_write());
                    leaf::remove(sizer,
                                 leaf_node,
                                 keys[i].btree_key(),
                                 repli_timestamp_t::distant_past,
                                 key_modification_proof_t::real_proof());
                }
                check_and_handle_underfull(sizer, &kv_location.buf,
                        &kv_location.last_buf, kv_location.superblock,
                        keys[i].btree_key(),
                        deletion_context->balancing_detacher());

                /* Here kv_location is destructed, which returns the superblock */
            }

            /* Reclaim the sindex superblock for the next deletion */
            sindex_superblock.init(superblock_promise.wait());
        }
    }

    {
        /* Start a transaction (2). */
        write_token_pair_t token_pair;
        store_view_t::new_write_token_pair(&token_pair);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_write(
            repli_timestamp_t::distant_past,
            2,
            write_durability_t::SOFT,
            &token_pair,
            &txn,
            &superblock,
            interruptor);

        /* Get the sindex block (2). */
        buf_lock_t sindex_block = acquire_sindex_block_for_write(
                superblock->expose_buf(),
                superblock->get_sindex_block_id());
        superblock->release();

        /* The root node should have been emptied at this point. Delete it */
        {
            buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                              sindex.superblock, access_t::write);
            real_superblock_t sindex_superblock(std::move(sindex_superblock_lock));
            if (sindex_superblock.get_root_block_id() != NULL_BLOCK_ID) {
                buf_lock_t root_node(sindex_superblock.expose_buf(),
                                     sindex_superblock.get_root_block_id(),
                                     access_t::write);
                {
                    // TODO (daniel): This assumption fails if erase_range has been
                    // used on the sindex. One more reason for why erase_range
                    // should die.
                    /* Guarantee that the root is actually empty. */
                    buf_read_t rread(&root_node);
                    const node_t *node = static_cast<const node_t *>(
                            rread.get_data_read());
                    if (node::is_internal(node)) {
                        const internal_node_t *root_int_node
                            = static_cast<const internal_node_t *>(
                                rread.get_data_read());
                        guarantee(root_int_node->npairs == 0);
                    }
                    /* If the root is a leaf we are good */
                }
                /* Delete the root */
                root_node.write_acq_signal()->wait_lazily_unordered();
                root_node.mark_deleted();
            }
            // TODO (daniel): There currently appears to be a stat block for
            // secondary indexes which is stupid. It makes them slower, since it
            // is being updated everytime a secondary index is being written to.
            // That should be changed. For now we just delete it here:
            if (sindex_superblock.get_stat_block_id() != NULL_BLOCK_ID) {
                buf_lock_t stat_block(sindex_superblock.expose_buf(),
                                      sindex_superblock.get_stat_block_id(),
                                      access_t::write);
                stat_block.write_acq_signal()->wait_lazily_unordered();
                stat_block.mark_deleted();
            }
            // TODO (daniel): ... and even an sindex block.
            if (sindex_superblock.get_sindex_block_id() != NULL_BLOCK_ID) {
                buf_lock_t sind_block(sindex_superblock.expose_buf(),
                                      sindex_superblock.get_sindex_block_id(),
                                      access_t::write);
                sind_block.write_acq_signal()->wait_lazily_unordered();
                sind_block.mark_deleted();
            }
        }
        /* Now it's safe to completely delete the index */
        buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                          sindex.superblock, access_t::write);
        sindex_superblock_lock.write_acq_signal()->wait_lazily_unordered();
        sindex_superblock_lock.mark_deleted();
        ::delete_secondary_index(&sindex_block, compute_sindex_deletion_id(sindex.id));
        size_t num_erased = secondary_index_slices.erase(sindex.id);
        guarantee(num_erased == 1);
    }
}

void store_t::set_sindexes(
        const std::map<std::string, secondary_index_t> &sindexes,
        buf_lock_t *sindex_block,
        std::set<std::string> *created_sindexes_out)
    THROWS_ONLY(interrupted_exc_t) {

    std::map<std::string, secondary_index_t> existing_sindexes;
    ::get_secondary_indexes(sindex_block, &existing_sindexes);

    for (auto it = existing_sindexes.begin(); it != existing_sindexes.end(); ++it) {
        if (!std_contains(sindexes, it->first)) {
            /* Mark the secondary index for deletion to get it out of the way
            (note that a new sindex with the same name can be created
            from now on, which is what we want). */
            bool success = mark_secondary_index_deleted(sindex_block, it->first);
            guarantee(success);

            /* Delay actually clearing the secondary index, so we can
            release the sindex_block now, rather than having to wait for
            clear_sindex() to finish. */
            coro_t::spawn_sometime(std::bind(&store_t::delayed_clear_sindex,
                                             this,
                                             it->second,
                                             drainer.lock()));
        }
    }

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        if (!std_contains(existing_sindexes, it->first)) {
            secondary_index_t sindex(it->second);
            {
                buf_lock_t sindex_superblock(sindex_block, alt_create_t::create);
                sindex.superblock = sindex_superblock.block_id();
                btree_slice_t::init_superblock(&sindex_superblock,
                                               std::vector<char>(),
                                               std::vector<char>());
            }

            secondary_index_slices.insert(it->second.id,
                                          new btree_slice_t(cache.get(),
                                                            &perfmon_collection,
                                                            it->first));

            sindex.post_construction_complete = false;

            ::set_secondary_index(sindex_block, it->first, sindex);

            created_sindexes_out->insert(it->first);
        }
    }
}

bool store_t::mark_index_up_to_date(const std::string &id,
                                    buf_lock_t *sindex_block)
    THROWS_NOTHING {
    secondary_index_t sindex;
    bool found = ::get_secondary_index(sindex_block, id, &sindex);

    if (found) {
        sindex.post_construction_complete = true;

        ::set_secondary_index(sindex_block, id, sindex);
    }

    return found;
}

bool store_t::mark_index_up_to_date(uuid_u id,
                                    buf_lock_t *sindex_block)
    THROWS_NOTHING {
    secondary_index_t sindex;
    bool found = ::get_secondary_index(sindex_block, id, &sindex);

    if (found) {
        sindex.post_construction_complete = true;

        ::set_secondary_index(sindex_block, id, sindex);
    }

    return found;
}

MUST_USE bool store_t::drop_sindex(
        const std::string &id,
        buf_lock_t sindex_block)
        THROWS_ONLY(interrupted_exc_t) {
    /* Remove reference in the super block */
    secondary_index_t sindex;
    if (!::get_secondary_index(&sindex_block, id, &sindex)) {
        return false;
    } else {
        /* Mark the secondary index as deleted */
        bool success = mark_secondary_index_deleted(&sindex_block, id);
        guarantee(success);

        /* Clear the sindex later. It starts its own transaction and we don't
        want to deadlock because we're still holding locks. */
        coro_t::spawn_sometime(std::bind(&store_t::delayed_clear_sindex,
                                         this,
                                         sindex,
                                         drainer.lock()));
    }
    return true;
}

MUST_USE bool store_t::mark_secondary_index_deleted(
        buf_lock_t *sindex_block,
        const std::string &id) {
    secondary_index_t sindex;
    bool success = get_secondary_index(sindex_block, id, &sindex);
    if (!success) {
        return false;
    }

    // Delete the current entry
    success = delete_secondary_index(sindex_block, id);
    guarantee(success);

    // Insert the new entry under a different name
    const std::string sindex_del_name = compute_sindex_deletion_id(sindex.id);
    sindex.being_deleted = true;
    set_secondary_index(sindex_block, sindex_del_name, sindex);
    return true;
}

MUST_USE bool store_t::acquire_sindex_superblock_for_read(
        const std::string &id,
        const std::string &table_name,
        superblock_t *superblock,
        scoped_ptr_t<real_superblock_t> *sindex_sb_out,
        std::vector<char> *opaque_definition_out,
        uuid_u *sindex_uuid_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();
    rassert(sindex_uuid_out != NULL);

    /* Acquire the sindex block. */
    buf_lock_t sindex_block
        = acquire_sindex_block_for_read(superblock->expose_buf(),
                                        superblock->get_sindex_block_id());
    superblock->release();

    /* Figure out what the superblock for this index is. */
    secondary_index_t sindex;
    if (!::get_secondary_index(&sindex_block, id, &sindex)) {
        return false;
    }

    if (opaque_definition_out != NULL) {
        *opaque_definition_out = sindex.opaque_definition;
    }
    *sindex_uuid_out = sindex.id;

    if (!sindex.is_ready()) {
        throw sindex_not_ready_exc_t(id, sindex, table_name);
    }

    buf_lock_t superblock_lock(&sindex_block, sindex.superblock, access_t::read);
    sindex_block.reset_buf_lock();
    sindex_sb_out->init(new real_superblock_t(std::move(superblock_lock)));
    return true;
}

MUST_USE bool store_t::acquire_sindex_superblock_for_write(
        const std::string &id,
        const std::string &table_name,
        superblock_t *superblock,
        scoped_ptr_t<real_superblock_t> *sindex_sb_out,
        uuid_u *sindex_uuid_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();
    rassert(sindex_uuid_out != NULL);

    /* Get the sindex block. */
    buf_lock_t sindex_block
        = acquire_sindex_block_for_write(superblock->expose_buf(),
                                         superblock->get_sindex_block_id());
    superblock->release();

    /* Figure out what the superblock for this index is. */
    secondary_index_t sindex;
    if (!::get_secondary_index(&sindex_block, id, &sindex)) {
        return false;
    }
    *sindex_uuid_out = sindex.id;

    if (!sindex.is_ready()) {
        throw sindex_not_ready_exc_t(id, sindex, table_name);
    }


    buf_lock_t superblock_lock(&sindex_block, sindex.superblock,
                               access_t::write);
    sindex_block.reset_buf_lock();
    sindex_sb_out->init(new real_superblock_t(std::move(superblock_lock)));
    return true;
}

void store_t::acquire_all_sindex_superblocks_for_write(
        block_id_t sindex_block_id,
        buf_parent_t parent,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    /* Get the sindex block. */
    buf_lock_t sindex_block = acquire_sindex_block_for_write(parent, sindex_block_id);

    acquire_all_sindex_superblocks_for_write(&sindex_block, sindex_sbs_out);
}

void store_t::acquire_all_sindex_superblocks_for_write(
        buf_lock_t *sindex_block,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    acquire_sindex_superblocks_for_write(
            boost::optional<std::set<std::string> >(),
            sindex_block,
            sindex_sbs_out);
}

void store_t::acquire_post_constructed_sindex_superblocks_for_write(
        buf_lock_t *sindex_block,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_NOTHING {
    assert_thread();
    std::set<std::string> sindexes_to_acquire;
    std::map<std::string, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        /* Note that this can include indexes currently being deleted.
        In fact it must include those indexes if they had been post constructed
        before, since there might still be ongoing transactions traversing the
        index despite it being under deletion.*/
        if (it->second.post_construction_complete) {
            sindexes_to_acquire.insert(it->first);
        }
    }

    acquire_sindex_superblocks_for_write(
            sindexes_to_acquire, sindex_block,
            sindex_sbs_out);
}

bool store_t::acquire_sindex_superblocks_for_write(
            boost::optional<std::set<std::string> > sindexes_to_acquire, //none means acquire all sindexes
            buf_lock_t *sindex_block,
            sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    std::map<std::string, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        if (sindexes_to_acquire && !std_contains(*sindexes_to_acquire, it->first)) {
            continue;
        }

        /* Getting the slice and asserting we're on the right thread. */
        btree_slice_t *sindex_slice = &(secondary_index_slices.at(it->second.id));
        sindex_slice->assert_thread();

        buf_lock_t superblock_lock(
                sindex_block, it->second.superblock, access_t::write);

        sindex_sbs_out->push_back(new
                sindex_access_t(get_sindex_slice(it->second.id), it->second, new
                    real_superblock_t(std::move(superblock_lock))));
    }

    //return's true if we got all of the sindexes requested.
    return sindex_sbs_out->size() == sindexes_to_acquire->size();
}

bool store_t::acquire_sindex_superblocks_for_write(
            boost::optional<std::set<uuid_u> > sindexes_to_acquire, //none means acquire all sindexes
            buf_lock_t *sindex_block,
            sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    std::map<std::string, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        if (sindexes_to_acquire && !std_contains(*sindexes_to_acquire, it->second.id)) {
            continue;
        }

        /* Getting the slice and asserting we're on the right thread. */
        btree_slice_t *sindex_slice = &(secondary_index_slices.at(it->second.id));
        sindex_slice->assert_thread();

        buf_lock_t superblock_lock(
                sindex_block, it->second.superblock, access_t::write);

        sindex_sbs_out->push_back(new
                sindex_access_t(get_sindex_slice(it->second.id), it->second, new
                    real_superblock_t(std::move(superblock_lock))));
    }

    //return's true if we got all of the sindexes requested.
    return sindex_sbs_out->size() == sindexes_to_acquire->size();
}

void store_t::check_and_update_metainfo(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const metainfo_t &new_metainfo,
        real_superblock_t *superblock) const
        THROWS_NOTHING {
    assert_thread();
    metainfo_t old_metainfo = check_metainfo(DEBUG_ONLY(metainfo_checker, ) superblock);
    update_metainfo(old_metainfo, new_metainfo, superblock);
}

metainfo_t
store_t::check_metainfo(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        real_superblock_t *superblock) const
        THROWS_NOTHING {
    assert_thread();
    region_map_t<binary_blob_t> old_metainfo;
    get_metainfo_internal(superblock->get(), &old_metainfo);
#ifndef NDEBUG
    metainfo_checker.check_metainfo(old_metainfo.mask(metainfo_checker.get_domain()));
#endif
    return old_metainfo;
}

void store_t::update_metainfo(const metainfo_t &old_metainfo,
                              const metainfo_t &new_metainfo,
                              real_superblock_t *superblock)
    const THROWS_NOTHING {
    assert_thread();
    region_map_t<binary_blob_t> updated_metadata = old_metainfo;
    updated_metadata.update(new_metainfo);

    rassert(updated_metadata.get_domain() == region_t::universe());

    buf_lock_t *sb_buf = superblock->get();
    clear_superblock_metainfo(sb_buf);

    for (region_map_t<binary_blob_t>::const_iterator i = updated_metadata.begin(); i != updated_metadata.end(); ++i) {
        vector_stream_t key;
        write_message_t wm;
        serialize(&wm, i->first);
        key.reserve(wm.size());
        DEBUG_VAR int res = send_write_message(&key, &wm);
        rassert(!res);

        std::vector<char> value(static_cast<const char*>(i->second.data()),
                                static_cast<const char*>(i->second.data()) + i->second.size());

        set_superblock_metainfo(sb_buf, key.vector(), value); // FIXME: this is not efficient either, see how value is created
    }
}

void store_t::do_get_metainfo(UNUSED order_token_t order_token,  // TODO
                              object_buffer_t<fifo_enforcer_sink_t::exit_read_t> *token,
                              signal_t *interruptor,
                              metainfo_t *out) THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_read(token,
                                &txn, &superblock,
                                interruptor,
                                false /* KSI: christ */);

    get_metainfo_internal(superblock->get(), out);
}

void store_t::
get_metainfo_internal(buf_lock_t *sb_buf,
                      region_map_t<binary_blob_t> *out)
    const THROWS_NOTHING {
    assert_thread();
    std::vector<std::pair<std::vector<char>, std::vector<char> > > kv_pairs;
    // TODO: this is inefficient, cut out the middleman (vector)
    get_superblock_metainfo(sb_buf, &kv_pairs);

    std::vector<std::pair<region_t, binary_blob_t> > result;
    for (std::vector<std::pair<std::vector<char>, std::vector<char> > >::iterator i = kv_pairs.begin(); i != kv_pairs.end(); ++i) {
        const std::vector<char> &value = i->second;

        region_t region;
        {
            inplace_vector_read_stream_t key(&i->first);
            archive_result_t res = deserialize(&key, &region);
            guarantee_deserialization(res, "region");
        }

        result.push_back(std::make_pair(region, binary_blob_t(value.begin(), value.end())));
    }
    region_map_t<binary_blob_t> res(result.begin(), result.end());
    rassert(res.get_domain() == region_t::universe());
    *out = res;
}

void store_t::set_metainfo(const metainfo_t &new_metainfo,
                           UNUSED order_token_t order_token,  // TODO
                           object_buffer_t<fifo_enforcer_sink_t::exit_write_t> *token,
                           signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    // KSI: Are there other places where we give up and use repli_timestamp_t::invalid?
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_write(repli_timestamp_t::invalid,
                                 1,
                                 write_durability_t::HARD,
                                 token,
                                 &txn,
                                 &superblock,
                                 interruptor);

    region_map_t<binary_blob_t> old_metainfo;
    get_metainfo_internal(superblock->get(), &old_metainfo);
    update_metainfo(old_metainfo, new_metainfo, superblock.get());
}

void store_t::acquire_superblock_for_read(
        object_buffer_t<fifo_enforcer_sink_t::exit_read_t> *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor,
        bool use_snapshot)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_read_t>::destruction_sentinel_t destroyer(token);
    wait_interruptible(token->get(), interruptor);

    cache_snapshotted_t cache_snapshotted =
        use_snapshot ? CACHE_SNAPSHOTTED_YES : CACHE_SNAPSHOTTED_NO;
    get_btree_superblock_and_txn_for_reading(
        general_cache_conn.get(), cache_snapshotted, sb_out, txn_out);
}

void store_t::acquire_superblock_for_backfill(
        object_buffer_t<fifo_enforcer_sink_t::exit_read_t> *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_read_t>::destruction_sentinel_t destroyer(token);
    wait_interruptible(token->get(), interruptor);

    get_btree_superblock_and_txn_for_backfilling(general_cache_conn.get(),
                                                 btree->get_backfill_account(),
                                                 sb_out, txn_out);
}

void store_t::acquire_superblock_for_write(
        repli_timestamp_t timestamp,
        int expected_change_count,
        const write_durability_t durability,
        write_token_pair_t *token_pair,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    acquire_superblock_for_write(timestamp,
                                 expected_change_count, durability,
                                 &token_pair->main_write_token, txn_out, sb_out,
                                 interruptor);
}

void store_t::acquire_superblock_for_write(
        repli_timestamp_t timestamp,
        int expected_change_count,
        write_durability_t durability,
        object_buffer_t<fifo_enforcer_sink_t::exit_write_t> *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_write_t>::destruction_sentinel_t destroyer(token);
    wait_interruptible(token->get(), interruptor);

    get_btree_superblock_and_txn(general_cache_conn.get(), write_access_t::write,
                                 expected_change_count, timestamp,
                                 durability, sb_out, txn_out);
}

/* store_view_t interface */
void store_t::new_read_token(object_buffer_t<fifo_enforcer_sink_t::exit_read_t> *token_out) {
    assert_thread();
    fifo_enforcer_read_token_t token = main_token_source.enter_read();
    token_out->create(&main_token_sink, token);
}

void store_t::new_write_token(object_buffer_t<fifo_enforcer_sink_t::exit_write_t> *token_out) {
    assert_thread();
    fifo_enforcer_write_token_t token = main_token_source.enter_write();
    token_out->create(&main_token_sink, token);
}
