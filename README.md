# crrd
This is a library to allow implementation of rrd (round robin database)
to allow collection of timed data from the past.

crrd.c is the library, crrd.h is the header for the library.

test.c is a self-contained test (it includes crrd.c)

gcc test.c  
./a.out  

runs the test suite (such as it is)

The idea is that the rrd is a fixed number of blocks, taken as a circular list with head and tail pointer. New entries come in, and the period (block of time) which the time is in is computed (simple chunking based on resolution). The resolution is usually in milliseconds, but this is hidden by type rrdt_t

If the block is in the future, fill values are entered until the new period is entered. If the block is the present one (that is, two entries are in the same period), a running average is performed. These are done by functions supplied by the user of library crrd.

This is extremely memory efficient and time efficient. Entries take constant time (generally) -- the only issue is if an entry is in the future it will take time proportional to the amount to time (in periods) to skip. As most uses have a constant rate of generation the time efficiency is O(1)

Because of this characteristic, multiple rrd structures can be updated concurrently. The result will be O(1) (constant) update time. Resolution and length are adjustable for each rrd, and these can be used to determine the most precise quantum queue to retrieve given data from.

The dbrrd_ functions are the primary way this is to be used. dbrrd_create to create a new rrd database, dbrrd_add to add elements at the current time. dbrrd_query to query for a timepoint and dbrrd_destroy to remove the database.

Again, all functions are in constant time, based on the number of "periods".  A period is a quantization of time (say minute, second, hour). Updates need to go to all periods being tracked. A query will look at all the periods (worst case). Each period probe is constant time. No dynamic memory allocation is used. Memory used is proportional to the number of samples within the periods (total over all the periods). In general, it is very difficult to use less memory to solve this. (but there may be algorithms of which I am unaware).

Use with TXGs

TXGs (transaction group numbers) are generated and stored. test.c has a test
case for this. When merged together, the txg structure keeps a range (low/high)
of transaction group numbers in a time period. The example uses 60 second,
one day and one year periods. dbrrd_query returns the actual txg range a time
entry was found in. If used as the beginning of a range, use the low value.
If the end is desired, use the high value. Filling 11 years of data at one
second intervals takes 18.6 seconds on my Thinkpad T460. Amortized over
a decade this is very reasonable.
