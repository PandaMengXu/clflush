#include <sys/time.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <fcntl.h>
#include <inttypes.h>

#ifndef __USE_GNU
#define __USE_GNU
#endif
#define _GNU_SOURCE

#include <sched.h>

#include <asm/unistd.h>

#ifndef __NR_flush_cache
#define __NR_flush_cache 365
#endif

#define NUM_CACHE_PARTITIONS 	8

static void usage(char *error) {
	fprintf(stderr, "Error: %s\n", error);
	fprintf(stderr,
		"Usage:\n"
		"	ca_spin [COMMON-OPTS] WCET PERIOD DURATION\n"
		"\n"
		"COMMON-OPTS = [-w] [-r 0/1] \n"
		"              [-p PARTITION/CLUSTER ] [-c CLASS]\n"
		"	       [-C num of cache partitions]"
		"\n"
		"WCET and PERIOD are milliseconds, DURATION is seconds.\n");
	exit(EXIT_FAILURE);
}

static void bail_out(const char *str) {
    fprintf(stderr, "%s\n", str);
    exit(EXIT_FAILURE);
}

static int be_migrate_to_cpu(pid_t pid, int target_cpu)
{
    cpu_set_t cpu_set;
    int ret;

    if (target_cpu < 0)
        return -1;

    CPU_ZERO(&cpu_set);
    CPU_SET(target_cpu, &cpu_set);

    if (pid == 0)
        pid = getpid();

    ret = sched_setaffinity(pid, sizeof(cpu_set), &cpu_set);

    return ret;
}

double cputime(void)
{
    struct timespec ts;
    int err;
    err = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    if (err != 0)
        perror("clock_gettime");
    return (ts.tv_sec + 1E-9 * ts.tv_nsec);
}

/* wall-clock time in seconds */
double wctime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

int sleep_nano(unsigned long long timeout)
{
    struct timespec delay;

    delay.tv_sec  = timeout / 1000000000L;
    delay.tv_nsec = timeout % 1000000000L;
    return nanosleep(&delay, NULL);
}

static busy_sleep(unsigned int seconds)
{
    double start = cputime();
    double end = cputime();

    while (end > (start + seconds)) {
        volatile int i, sum = 0;
        for (i = 0; i < 100000; ++i) {
            sum += i;
        }

        end = cputime();
    }
}

static void do_flush_cache(volatile void *p) {
	asm volatile ("clflush (%0)" :: "r"(p));
}

static long long rdtsc(void) {
	unsigned long a, d;
	asm volatile ("rdtsc" : "=a" (a), "=d" (d));
	return a | ((long long)d << 32);
}

static char* progname;
static int job_no;
//////////////////////////////////////////

#define KB_IN_CACHE_PARTITION	1024

#define CACHELINE_SIZE 64
#define INTS_IN_CACHELINE (CACHELINE_SIZE / sizeof(int))
#define CACHELINES_IN_1KB (1024 / sizeof(cacheline_t))
#define INTS_IN_1KB (1024 / sizeof(int))
#define INTS_IN_CACHELINE (CACHELINE_SIZE / sizeof(int))

typedef struct cacheline {
	int line[INTS_IN_CACHELINE];
} __attribute__((aligned(CACHELINE_SIZE))) cacheline_t;

static cacheline_t *arena = NULL;
static int loops = 10;
static int flush_cache = 0;
static int write_access = 0;

#define UNCACHE_DEV "/dev/litmus/uncache"
static cacheline_t* allocate_arena(size_t size, int use_huge_pages, int use_uncache_pages) {

	int flags = MAP_PRIVATE | MAP_POPULATE;
	cacheline_t* arena = NULL;
	int fd;

	if (use_huge_pages) {
		flags |= MAP_HUGETLB;
	}

	if (use_uncache_pages) {
		fd = open(UNCACHE_DEV, O_RDWR|O_SYNC);
		if (fd == -1) {
			bail_out("Failed to open uncache device.");
		}
	}
	else {
		fd = -1;
		flags |= MAP_ANONYMOUS;
	}

	arena = mmap(0, size, PROT_READ | PROT_WRITE, flags, fd, 0);

	if (use_uncache_pages) {
		close(fd);
	}

	assert(arena);

	return arena;
}

static void dealloc_arena(cacheline_t* mem, size_t size) {
	int ret = munmap((void*)mem, size);

	if (ret != 0) {
		bail_out("munmap() error");
	}
}

static int randrange(int min, int max) {
	int limit = max - min;
	int divisor = RAND_MAX / limit;
	int retval;

	do {
		retval = rand() / divisor;
	} while(retval == limit);

	retval += min;

	return retval;
}

static void init_arena(cacheline_t* arena, size_t size, int shuffle) {
	int i;

	size_t num_arena_elem = size / sizeof(cacheline_t);

	if (shuffle) {

		for (i = 0; i < num_arena_elem; i++) {
			int j;
			for(j = 0; j < INTS_IN_CACHELINE; ++j) {
				arena[i].line[j] = i;
			}
		}

		while(1 < i--) {
			int j = randrange(0, i);

			cacheline_t temp = arena[j];
			arena[j] = arena[i];
			arena[i] = temp;
		}
	}
	else {
		for (i = 0; i < num_arena_elem; i++) {
			int j;
			int next = (i + 1) % num_arena_elem;
			for(j = 0; j < INTS_IN_CACHELINE; ++j) {
				arena[i].line[j] = next;
			}
		}
	}
}

static cacheline_t* cacheline_start(int wss, int shuffle) {
	return arena + (shuffle * randrange(0, ((wss * 1024) / sizeof(cacheline_t))));
}

static int cacheline_walk(cacheline_t *mem, int wss) {
	int sum, i, next;

	int numlines = wss * CACHELINES_IN_1KB;

	sum = 0;

	next = mem - arena;

	for (i = 0; i < numlines; i++) {
		next = arena[next].line[0];
		sum += next;
        if (write_access) {
            arena[next].line[1] = sum;
        }
	}

	return sum;
}

static int loop_once(int wss, int shuffle) {
	cacheline_t *mem;
	int temp;
	
	mem = cacheline_start(wss, shuffle);
	temp = cacheline_walk(mem, wss);

	return temp; 
}


static double flush_arena(int wss)
{
    struct timespec ts1, ts2;
    double start, end;
	int i;
	int numlines = wss * CACHELINES_IN_1KB;

    syscall(__NR_flush_cache, &ts1, &ts2);
    start = ts1.tv_sec + ts1.tv_nsec / 1e9;
    end = ts2.tv_sec + ts2.tv_nsec / 1e9;

    printf("%.9f - %.9f 0x%p\n", start, end, arena);
    fflush(stdout);

/*
    start = cputime();
	for (i = 0; i < numlines; ++i) {
		do_flush_cache((void*)arena[i].line);
	}
    end = cputime();
*/
    return end - start;
}

#define ONE_SEC 1000000000L
#define BS	1024
static int job(int wss, int shuffle)
{
	register unsigned int iter = 0;
    double start, start2, end;
    double flush_time = 0.0;

    start = cputime();

    if (flush_cache == 1) {
		flush_time = flush_arena(wss);
	}
 
    start2 = cputime();

	while(iter++ < loops) {
		/* each cp takes 100us if cache hit */
		loop_once(wss, shuffle);
	}

    end = cputime();
    printf("[WCET] job_no: %d wcet(incl): %.9fs wcet(excl): %.9fs "
           "flush: %.9f flush_actual: %.9f\n", job_no, 
        (end - start), (end - start2), (start2 - start), flush_time);
    fflush(stdout);

	return 1;
}

static void initialize(size_t arena_size, int shuffle) {
    double start, end;

    start = cputime();
 
	arena = allocate_arena(arena_size, 0, 0);
	init_arena(arena, arena_size, shuffle);
	
    end = cputime();
    printf("init: %.9f s\n", (end - start));
    fflush(stdout);
}

#define OPTSTR "p:r:l:S:j:s:m:J:fw"
int main(int argc, char** argv)
{
	int ret;
	int migrate = 0;
	int cluster = 0;
	int opt;
	double duration = 0, start = 0;

	int wss = 0;
	int shuffle = 1;
	size_t arena_size;
    int jobs = 1000;
    int sleep_seconds = 1;

    int new_cluster = -1;
    int mig_jobno = -1;

    int cpu;

	progname = argv[0];

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'p':
			cluster = atoi(optarg);
			migrate = 1;
			break;
		case 'S':
			wss = atoi(optarg);
			break;
		case 'r':
			shuffle = atoi(optarg);
			if(shuffle) {
				shuffle = 1;
			}
			break;
		case 'l':
			loops = atoi(optarg);
			break;
        case 'j':
            jobs = atoi(optarg);
            break;
        case 's':
            sleep_seconds = atoi(optarg);
            break;
        case 'm':
            new_cluster = atoi(optarg);
            break;
        case 'J':
            mig_jobno = atoi(optarg);
            break;
        case 'f':
            flush_cache = 1;
            break;
        case 'w':
            write_access = 1;
            break;
		case ':':
			usage("Argument missing.");
			break;
		case '?':
		default:
			usage("Bad argument.");
			break;
		}
	}

    if (wss <=0) 
        usage("wss missing");

	srand(0);

	if (argc - optind < 0)
		usage("Arguments missing.");

	if (migrate) {
		ret = be_migrate_to_cpu(0, cluster); //be_migrate_to_domain(cluster);
		if (ret < 0)
			bail_out("could not migrate to target partition or cluster.");
	}

    while ((cpu=sched_getcpu()) != cluster) {
        printf("CPU=%d\n", cpu);
        sleep(1);
    }

	printf("wss=%dKB\n", wss);
	arena_size = wss * 1024;

	initialize(arena_size, shuffle);

	start = wctime();

	while (job(wss, shuffle))
	{
        if (sleep_seconds > 0)
            busy_sleep(sleep_seconds);

        if (new_cluster != -1 && job_no == mig_jobno) {
            ret = be_migrate_to_cpu(0, new_cluster);
            if (ret < 0)
			    bail_out("could not migrate to target partition or cluster.");
        }       

		job_no++;
        if (jobs > 0 && job_no >= jobs)
            break;
	};

	dealloc_arena(arena, arena_size);

	printf("ca_spin finished.\n");

	return 0;
}

