// known issue(fix in next commit):
// -1. During each TEST_COUNT, we should add a `usleep`(25000us test lowest
// currently), or `mutex_idx` won't work,
//     which futher cause seg fault (OOB access of `time_res`) (haven't figured
//     why), this issue got connection with issue 4.
// 1. cur_thread_cnt should start from 1, or it cause the failure of first reset
// of time_res
// 2. Typo "avg result for each thread" should be "avg result of each thread"
// 3. (data type of `test_res_avg` should change to ull)Change measurement time
//    scale from ns to us or ms, which means impl of `time_diff_ns` should be
//    updated too. (I think gettimeofday is proper one)
// 4. We should wait for a while after each thread set test case, since the
// state of TCP stack, TIME_WAIT,
//    too many of this (resources to be released) will cause resource shortage
//    of TCP stack, which further causing the failure of `connect`
// 5. Remove inclusion of semaphore since we may no longer use it
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define COND_RD_WAIT_IN_SEC 1
#define MAX_THREAD 50
#define MAX_POSSIBLE_LENGTH \
    32  // this correspond to random-length dummy string, `msg_dum`
#define TARGET_PORT 12345

// we should change the measurement scale since ns is too small, which
// cause result overflow, then we can increase this
#define TEST_COUNT 50

#define RESULT_FILE_NAME "kecho_perf.txt"
#define unlikely(x) __builtin_expect(!!(x), 0)
#define TEST_WAIT_INTERVAL_MS 100000

// TODO: dummy msg shouldn't be a fixed-length string, its length should be
// random to get more accurate measurement result
const char *msg_dum = "dummy";

// create once, fit all test case, then we dont need malloc
pthread_t pt[MAX_THREAD];

// blocks all threads before they are all ready to send message
int rd_to_go = 0;

pthread_mutex_t mutex_idx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_wa_cond = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_wait_all = PTHREAD_COND_INITIALIZER;

long time_res[MAX_THREAD] = {
    0};       // create once, fit all test case, then we dont need malloc
int idx = 0;  // for indexing `time_res`

static inline long time_diff_ns(struct timespec *start, struct timespec *end)
{
    long delta = end->tv_nsec - start->tv_nsec;

    if ((end->tv_sec - start->tv_sec) > 1)
        return -1;  // calculation of fabonacci spends too much time (at least 2
                    // second), won't happen at usual

    return delta;
}

void *worker(void *arg)
{
    int sock_fd, rt;
    char dummy[MAX_POSSIBLE_LENGTH];
    long time_diff;
    struct timespec start, end;

    struct timeval now;
    struct timespec timeout;

    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + COND_RD_WAIT_IN_SEC;
    timeout.tv_nsec = now.tv_usec * 1000;

    pthread_mutex_lock(&mutex_wa_cond);
    while (!rd_to_go) {
        // wait till all threads created
        rt = pthread_cond_timedwait(&cond_wait_all, &mutex_wa_cond, &timeout);
    }
    pthread_mutex_unlock(&mutex_wa_cond);
    if (rt != 0) {
        printf("cond_wait failed with error code: %d\n", rt);
        exit(-1);
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        goto sock_init_fail;
    }

    struct sockaddr_in info;
    bzero(&info, sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = inet_addr("127.0.0.1");
    info.sin_port = htons(TARGET_PORT);

    if (connect(sock_fd, &info, sizeof(info)) == -1) {
        goto connect_fail;
    }

    clock_gettime(CLOCK_REALTIME, &start);
    send(sock_fd, msg_dum, strlen(msg_dum), 0);
    recv(sock_fd, &dummy, MAX_POSSIBLE_LENGTH, 0);
    clock_gettime(CLOCK_REALTIME, &end);

    close(sock_fd);


    time_diff = time_diff_ns(&start, &end);
    if (unlikely(time_diff == -1)) {
        goto timeout;
    }

    pthread_mutex_lock(&mutex_idx);
    // printf("\nmy idx is %d\n", idx);
    time_res[idx] += time_diff;
    idx++;

    pthread_mutex_unlock(&mutex_idx);
    puts("going to exit");
    pthread_exit(NULL);

timeout:
    puts("Socket transmission timeout (over one second)");
    exit(-1);

sock_init_fail:
    perror("Socket create failed");
    exit(-1);

connect_fail:
    perror("Socket connect failed");
    exit(-1);
}

void create_threads(int thread_cnt)
{
    int status;

    for (int i = 0; i < thread_cnt; i++) {
        status = pthread_create(&pt[i], NULL, worker, NULL);
        if (status) {
            puts("Thread creation failure");
            exit(-1);
        }
    }
}

int main(void)
{
    printf("pid: %d\n", getpid());
    long test_res_avg = 0;  // if TEST_COUNT is big enough, we should concern
                            // the possibility of overflow
    int status = 0;
    FILE *fd_perf;
    fd_perf = fopen(RESULT_FILE_NAME, "w");
    if (!fd_perf) {
        status = -1;
        goto fopen_fail;
    }

    for (int cur_thread_cnt = 0; cur_thread_cnt < MAX_THREAD;
         cur_thread_cnt++) {
        for (int i = 0; i < TEST_COUNT; i++) {
            create_threads(cur_thread_cnt + 1);

            pthread_mutex_lock(&mutex_wa_cond);

            rd_to_go = 1;

            // all threads are ready, let's start bombing kecho
            pthread_cond_broadcast(&cond_wait_all);

            pthread_mutex_unlock(&mutex_wa_cond);

            for (int x = 0; x < cur_thread_cnt;
                 x++) {  // waiting for all threads to finish the measurement
                pthread_join(pt[x],
                             NULL);  // all threads should done eventually, no
                                     // deadlock-like stuff
            }
            puts("done single test loop");
            usleep(TEST_WAIT_INTERVAL_MS);
            // reset thread-related var
            rd_to_go = 0;
            idx = 0;
        }

        // result calculation
        for (int i = 0; i < cur_thread_cnt; i++) {
            time_res[i] =
                time_res[i] / TEST_COUNT;  // avg result for each thread
            test_res_avg +=
                time_res[i];  // summation of avg result of all threads
            // printf("%ld\n", test_res_avg);
        }
        test_res_avg /= cur_thread_cnt + 1;  // avg result of kecho

        test_res_avg /= 1000;  // turn unit of result from ns to us

        fprintf(fd_perf, "%d %ld\n", cur_thread_cnt + 1, test_res_avg);

        // reset for next measurement
        for (int i = 0; i < cur_thread_cnt; i++) {
            time_res[i] = 0;
        }
        test_res_avg = 0;
    }

    goto success;

fopen_fail:
    perror("Failed to create result file");

success:
    fclose(fd_perf);
    return status ?: 0;
}