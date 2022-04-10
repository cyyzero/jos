#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

// #define SOL

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void 
barrier(/*int no*/)
{
  int n, lRound;
  pthread_mutex_lock(&bstate.barrier_mutex);
  n = bstate.nthread++;
  // record the round
  lRound = bstate.round;
//   fprintf(stderr, "THREAD %d, round %d :n = %d\n", no, bstate.round, n);

  if (n == nthread-1) {
    // fprintf(stderr, "THREAD %d, round %d :round = %d\n",  no, bstate.round,bstate.round);
    bstate.nthread = 0;
    ++bstate.round;
    pthread_mutex_unlock(&bstate.barrier_mutex);
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    // fprintf(stderr, "THREAD %d, round %d  is waiting\n", no, bstate.round);
    // if round is changed, it is safe to wake up. Avoid spurious wake-ups
    while (lRound == bstate.round) {
        pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    }
    // fprintf(stderr, "THREAD %d, round %d is wakon up\n", no, bstate.round);
    pthread_mutex_unlock(&bstate.barrier_mutex);
  }
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier(n);
    // fprintf(stderr, "\nTHREAD %ld : ORIGIN nthread = %d, round = %d\n", n, bstate.nthread, bstate.round);
    usleep(random() % 100);
  }
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
