# clflush
This repo. holds the userspace code and modified kernel to run the experiment for the stackoverflow question:
http://stackoverflow.com/questions/35900401/how-does-clflush-work-for-an-address-that-is-not-in-cache-yet?noredirect=1#comment59474580_35900401


The litmus folder contains the code for the kernel; 
The system call sys_flush_cache is in flie ./litmus/litmus/litmus.c

The program folder contains the code to run the userspace program to call the sys_flush_cache to flush the cache for the program.
An example to create a 20MB array for the program is as follows:
./ca_spin -p 1 -S 20480 -l 100 -j 10 -r 1 -s 100 -f

The meaning of each option is as follows:
-p: which core the program is going to run
-S: the size of the array this program will create (in KB)
-l: the number of loops the array will be iterated before we flush the cache for the task;
-j: the number of times  we will flush the cache for the task; each time we flush the cache for the task, we call it a job.
-r: Do we iterate the array in random order
-s: The interval the program will sleep before it run the next job
-f: It's read only, and we will flush cache
