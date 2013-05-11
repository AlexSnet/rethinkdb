#!/usr/bin/python
import sys, os, time, random, signal
from optparse import OptionParser

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'drivers', 'python')))
import rethinkdb as r

random.seed()

parser = OptionParser()
parser.add_option("--host", dest="host_port", metavar="HOST:PORT", default="", type="string")
parser.add_option("--table", dest="db_table", metavar="DB.TABLE", default="test.test", type="string")
parser.add_option("--writes", dest="writes", metavar="WRITES", default=3, type="int")
parser.add_option("--deletes", dest="deletes", metavar="DELETES", default=2, type="int")
parser.add_option("--reads", dest="reads", metavar="READS", default=5, type="int")
parser.add_option("--sindex-reads", dest="sindex_reads", metavar="SINDEX_READS", default=0, type="int")
parser.add_option("--batch-size", dest="batch_size", metavar="BATCH_SIZE", default=100, type="int")
parser.add_option("--output", dest="output_file", metavar="FILE", default="", type="string")
parser.add_option("--sindex", dest="sindexes", metavar="COUNT", default=[], type="string", action="append")
# TODO: add options --min-key, --max-key, and --client-id, which will allow an existing database of continguous keys to have their 'ownership' distributed to a set of stress clients
(options, args) = parser.parse_args()

if len(args) != 0:
    raise RuntimeError("No positional arguments supported")

if options.sindex_reads != 0 and len(options.sindexes) == 0:
    raise RuntimeError("Cannot do sindex reads if there are no sindexes")

if len(options.output_file) == 0:
    raise RuntimeError("--output must be specified")

if ":" not in options.host_port:
    raise RuntimeError("Incorrect host:port format in --host option")

if "." not in options.db_table:
    raise RuntimeError("Incorrect db.table format in --table option")

(host, port) = options.host_port.split(":")
port = int(port)

(db, table) = options.db_table.split(".")

connection = r.connect(host, port, db)

if db not in r.db_list().run(connection):
    raise RuntimeError("Database does not exist: " + db)

if table not in r.db("test").table_list().run(connection):
    raise RuntimeError("Table does not exist: " + table)

extant_keys = set()

operation_stats = { "read": 0,
                    "write": 0,
                    "sindex_read": 0,
                    "delete": 0 }

stats_file = open(options.output_file, "w+")

def write_stats(timestamp):
    global operation_stats
    global stats_file

    data = [str(timestamp)]
    for op, value in operation_stats.items():
        data.extend([op, str(value)])
        operation_stats[op] = 0

    stats_file.write(",".join(data) + "\n")

# TODO: verify results of all operations

def do_write():
    global operation_stats
    global extant_keys
    global options

    do_write.counter += 1
    if do_write.counter >= options.batch_size:
        write_data = [ ]
        for i in range(options.batch_size):
            write_data.append({ "value": random.randint(0, 10000) })
        result = r.table(table).insert(write_data).run(connection)
        for key in result["generated_keys"]:
            extant_keys.add(key)
        operation_stats["write"] += do_write.counter
        do_write.counter = 0
do_write.counter = 0

def do_read():
    global operation_stats
    global extant_keys

    key = random.sample(extant_keys, 1)[0]
    r.table(table).get(key).run(connection)
    operation_stats["read"] += 1

def do_sindex_read():
    # TODO: implement this once sindex reads are supported in the python driver
    #global operation_stats
    #global extant_keys
    #global sindexes
    #sindex = random.choice(sindexes)
    #sindex_value = random_sindex_value(sindex)
    #r.table(table).get_all(sindex, sindex_value).run(connection)
    #operation_stats["sindex_read"] += 1
    return

def do_delete():
    global operation_stats
    global extant_keys

    key = random.sample(extant_keys, 1)[0]
    r.table(table).get(key).delete().run(connection)
    extant_keys.remove(key)
    operation_stats["delete"] += 1

# Probabilities for each operation type
operation_weights = [ ("read", options.reads, do_read),
                      ("write", options.writes, do_write),
                      ("sindex_read", options.sindex_reads, do_sindex_read),
                      ("delete", options.deletes, do_delete) ]

total_op_weight = sum(weight for (name, weight, fn) in operation_weights)

# Batch writes by just keeping track of how many we've tried to do
num_writes = 0

def do_operation():
    global operation_weights
    global total_op_weight
    global extant_keys
    global num_writes
    global options

    # If the table is empty, do a write
    if len(extant_keys) == 0:
        do_write()
        return

    # Do a weighted roll on the operation to perform, probably a cleaner way to do this
    r = random.randint(1, total_op_weight)
    choice = ""
    for (op, weight, fn) in operation_weights:
        r -= weight
        if r <= 0:
            fn()
            return

    raise RuntimeError("Did not choose an operation to run")

# Set up interruption
def interrupt_handler(signal, frame):
    global stats_file
    if stats_file.closed:
        print "Warning, stats file closed"
    else:
        write_stats(time.time())
        stats_file.close()
    exit(0)

try:
    signal.signal(signal.SIGINT, interrupt_handler)

    # Synchronize with parent stress process (so all clients start at the same time)
    sys.stdout.write("ready\n")
    sys.stdout.flush()

    if sys.stdin.readline().strip() != "go":
        raise RuntimeError("unexpected message from parent")

    # Run until interrupted
    stats_time = time.time() # write an initial stats so we have the start point
    consecutive_errors = 0
    while True:
        # Append stats to the output file every second
        if time.time() >= stats_time:
            write_stats(stats_time)
            stats_time = stats_time + 1

        try:
            do_operation()
            consecutive_errors = 0
        except r.RqlRuntimeError as ex:
            # Put a small sleep in here to keep from saturating cpu in error conditions
            consecutive_errors += 1
            if consecutive_errors >= 5:
                time.sleep(0.5)
except SystemExit:
    # This is the normal path for exiting the stress client
    pass
