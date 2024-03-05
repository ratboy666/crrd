# crrd
This is a library to allow implementation of rrd (round robin database)
to allow collection of timed data from the past.

crrd.c is the library, crrd.h is the header for the library.

test.c is a self-contained test (it includes crrd.c)

gcc test.c  
./a.out  

runs the test suite (such as it is)

The idea is that the rrd is a fixed number of blocks, taken as a circular
list with head and tail pointer. New entries come in, and the period (block of time) which the time is in is computed (simple chunking based on resolution). The resolution is usually in milliseconds, but this is hidden by type rrdt_t

If the block is in the future, zero values are entered until the new period is entered. If the block is the present one (that is, two entries are in the same period), a running average is performed. This is done by a function supplied
by the user of library crrd.

This is extremely memory efficient and time efficient. Entries take constant time (generally) -- the only issue is if an entry is in the future it will take
time proportional to the amount to time (in periods) to skip. As most uses have a constant rate of generation the time efficiency is O(1)

Because of this characteristic, multiple rrd structures can be updated concurrently. The result will be O(1) (constant) update time. resolution and length are adjustable for each rrd, and these can be used to determine the most precise queue to retrieve given data from.

test.c has the prototype of the the "multi rrd" functions - create, destroy, add and find.

