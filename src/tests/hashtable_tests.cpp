#include "misc_lib.h"
#include "print_stats.h"
#include "sync.h"
#include "tests.hpp"

// For extern theta_arg
#include "Application.hpp"


#include "distribution/pregen/mem.h"

namespace kmercounter {
struct kmer {
  char data[KMER_DATA_LENGTH];
};

extern void get_ht_stats(Shard *, BaseHashTable *);

// #define HT_TESTS_BATCH_LENGTH 32
#define HT_TESTS_BATCH_LENGTH 128

uint64_t HT_TESTS_HT_SIZE = (1 << 28);
uint64_t HT_TESTS_NUM_INSERTS;

#define HT_TESTS_MAX_STRIDE 2


extern Configuration config;
volatile uint64_t *mem;

typedef struct insert_thread_data {
    int thread_id;
    //uint64_t seed;
    uint64_t* data;
    uint64_t num;
    //uint64_t start;
    //uint64_t end;
    
    KmerHashTable* ktable;

    uint64_t count;
} insert_data;
void* insert_chunk(void* data)
{
    //unsigned cpu;
    //uint64_t t_start, t_end;

    insert_data* td = (insert_data*) data;
    td->count = 0;
    auto k = 0;

    __attribute__((aligned(64))) struct kmer kmers[HT_TESTS_BATCH_LENGTH] = {0};
    //printf("Thread %d: i goes from %lu - %lu\n", td->thread_id, td->start, td->end-1);
    //printf("Thread %d: i goes from %lu - %lu\n", td->thread_id, 0, td->num-1);

    //t_start = RDTSC_START();
    for (uint64_t i = 0; i < td->num; i++) {
        #ifdef INSERTION_CHUNKING
            *((uint64_t *)&kmers[k].data) = td->data[i];
        #else
            *((uint64_t *)&kmers[k].data) = td->data[INS_THREADS*i];
        #endif
        //printf("%lu\n", mem[i]);
        
        //td->ktable->insert((void *)&kmers[k]);
        k = (k + 1) & (HT_TESTS_BATCH_LENGTH - 1);
        ++(td->count);
    }
    /*t_end = RDTSCP();
    getcpu(&cpu, NULL);
    printf("Thread %d: Ins data in %lu cycles (%f ms) on CPU #%d\n", td->thread_id, t_end-t_start, (double)(t_end-t_start) * one_cycle_ns / 1000000.0, cpu);*/
    
    return NULL;
  // printf("FILE: \"%s\":%d\n",__FILE__, __LINE__);
  //return count;
}
uint64_t SynthTest::synth_run2(KmerHashTable *ktable) {
    cpu_set_t cpuset;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    uint64_t count = 0;
    pthread_t thread[INS_THREADS];
    //std::thread* thread = new std::thread[INS_THREADS];
    insert_data td[INS_THREADS];
    uint64_t start, end;
    int rc;

    for(int i = 0, cpu = 0; i < INS_THREADS; ++i, cpu = (cpu+1)%std::thread::hardware_concurrency()) {
        td[i].thread_id = i;

        start = ((double)i/INS_THREADS)*config.data_length;
        end = ((double)(i+1)/INS_THREADS)*config.data_length;
        td[i].num = end - start;
        #ifdef INSERTION_CHUNKING
            td[i].data = start + (uint64_t*) mem;
        #else
            td[i].data = i + (uint64_t*) mem;
        #endif

        CPU_ZERO(&cpuset);
        if(cpu == 1)
        {
          ++cpu;
        }
        CPU_SET(cpu, &cpuset);
        rc = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);//thread[i], sizeof(cpu_set_t), &cpuset);
        if (rc) {
          perror("Error:unable to set thread affinity");
          exit(-1);
        }

        //td[i].start = ((double)i/INS_THREADS)*data_len;
        //td[i].end = ((double)(i+1)/INS_THREADS)*data_len;
        //td[i].data = (uint64_t*) mem;

        rc = pthread_create(&thread[i], &attr, insert_chunk, (void *)&td[i]);
        if (rc) {
          perror("Error:unable to create thread");
          exit(-1);
        }

        /*CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(thread[i], sizeof(cpu_set_t), &cpuset);
        if (rc) {
          perror("Error:unable to set thread affinity");
          exit(-1);
        }*/
    }

    for(int i = 0; i < INS_THREADS; i++) {
        rc = pthread_join(thread[i], NULL);
        if (rc) {
          perror("thread join failed");
          exit(-1);
        }
        count += td[i].count;
    }
    

    //delete[] threads;
    return count;
}
uint64_t SynthTest::synth_run(BaseHashTable *ktable, uint8_t start) {
  uint64_t count = HT_TESTS_NUM_INSERTS * start;
  auto k = 0;
  auto i = 0;
  struct xorwow_state _xw_state;

  xorwow_init(&_xw_state);

  __attribute__((aligned(64))) struct kmer kmers[HT_TESTS_BATCH_LENGTH] = {0};
  __attribute__((aligned(64))) struct Item items[HT_TESTS_BATCH_LENGTH] = {0};
  __attribute__((aligned(64))) uint64_t keys[HT_TESTS_BATCH_LENGTH] = {0};

  for (i = 0u; i < HT_TESTS_NUM_INSERTS/*config.data_length*/; i++) {
#if defined(SAME_KMER)
    //*((uint64_t *)&kmers[k].data) = count & (32 - 1);
    *((uint64_t *)&kmers[k].data) = 32;
    *((uint64_t *)items[k].key()) = 32;
    *((uint64_t *)items[k].value()) = 32;
    keys[k] = 32;
#elif defined(XORWOW)
#warning "Xorwow rand kmer insert"
    *((uint64_t *)&kmers[k].data) = xorwow(&_xw_state);
#elif defined(ZIPF)  //
    *((uint64_t *)&kmers[k].data) = zg->next();
    // printf("%ld\n", zg->next());
#elif defined(MINE)
    //*((uint64_t *)&kmers[k].data) = mine::next1(rand_);
    *((uint64_t *)&kmers[k].data) =
        mine::next(0.33, -1.47, -0.272179, HT_TESTS_NUM_INSERTS, rand_);
    // printf("%ld\n", mine::next(0.36, -1.47, -0.296, rand_));
    // printf("%lu\n", mine::next2(rand_));
#elif defined(PREGEN)
    *((uint64_t *)&kmers[k].data) = mem[i];
    //printf("%lu\n", mem[i]);
#elif defined(HEADER)
    *((uint64_t *)&kmers[k].data) =
        next();  // ZipfGen(HT_TESTS_NUM_INSERTS, theta_arg,
                 // HT_TESTS_NUM_INSERTS);//next();//1;//
    //printf("%lu\n", next());
#else
    *((uint64_t *)&kmers[k].data) = count;//mem[i];
    *((uint64_t *)items[k].key()) = count;
    *((uint64_t *)items[k].value()) = count;
    keys[k] = count;
#endif
    // printf("[%s:%d] inserting i= %d, data %lu\n", __func__, start, i, count);
    // printf("%s, inserting i= %d\n", __func__, i);
    // ktable->insert((void *)&kmers[k]);
    // printf("->Inserting %lu\n", count);
    count++;
    // ktable->insert((void *)&items[k]);
    ktable->insert((void *)&keys[k]);

    // ktable->insert_noprefetch((void *)&keys[k]);
    k = (k + 1) & (HT_TESTS_BATCH_LENGTH - 1);
    //#if defined(SAME_KMER) || defined(ZIPF) || defined(MINE)
    ++count;  // count++;
    //#endif
  }
  // flush the last batch explicitly
  printf("%s calling flush queue\n", __func__);
  ktable->flush_queue();
  printf("%s: %p\n", __func__, ktable->find(&kmers[k]));
  return i;
}

uint64_t seed2 = 123456789;
inline uint64_t PREFETCH_STRIDE = 64;

void SynthTest::synth_run_exec(Shard *sh, BaseHashTable *kmer_ht) {
  uint64_t num_inserts = 0;
  uint64_t t_start, t_end;

  printf("[INFO] Synth test run: thread %u, ht size: %lu, insertions: %lu\n",
         sh->shard_idx, HT_TESTS_HT_SIZE, HT_TESTS_NUM_INSERTS);

  for (auto i = 1; i < HT_TESTS_MAX_STRIDE; i++) {
// Depending on selected macro, initialize different datas
#if defined(ZIPF)
    printf("theta_arg = %f\n", theta_arg);
    Base_ZipfGen *zg =
        ZipfGen(HT_TESTS_NUM_INSERTS, theta_arg, HT_TESTS_NUM_INSERTS);
#elif defined(MINE)
    Rand *rand_ = new Rand();
#elif defined(HEADER)
    ZipfGen(HT_TESTS_NUM_INSERTS, theta_arg, HT_TESTS_NUM_INSERTS);
#else
    // printf("here\n");
    // uint64_t t_start = RDTSC_START();

    t_start = RDTSC_START();
    mem = generate(config.data_length, config.data_range, config.theta, (uint64_t)(config.data_length*config.data_range*config.theta)%(1UL << 48));
    //mem = generate(HT_TESTS_NUM_INSERTS, HT_TESTS_NUM_INSERTS, theta_arg, HT_TESTS_NUM_INSERTS);
    t_end = RDTSCP();
    printf("[INFO] Pre-generated data in %lu cycles (%f ms) at rate of %lu cycles/element\n", t_end-t_start, (double)(t_end-t_start) * one_cycle_ns / 1000000.0, (t_end-t_start)/HT_TESTS_NUM_INSERTS);
    // mem = generate(HT_TESTS_NUM_INSERTS, HT_TESTS_NUM_INSERTS, theta_arg,
    // HT_TESTS_NUM_INSERTS); uint64_t t_end = RDTSCP(); printf("Cycles to
    // generate: %lu (%f ms)\t Cycles per gen: %f\n", t_end-t_start,
    // (double)(t_end-t_start)* one_cycle_ns / 1000000.0,
    // (t_end-t_start)/(HT_TESTS_NUM_INSERTS*1.0));
#endif
    t_start = RDTSC_START();

    // PREFETCH_QUEUE_SIZE = i;

    // PREFETCH_QUEUE_SIZE = 32;
    #ifdef MULTITHREAD_INSERTION
        num_inserts = synth_run2(kmer_ht, sh->shard_idx);
    #else
        num_inserts = synth_run(kmer_ht, sh->shard_idx);
    #endif

    t_end = RDTSCP();
    printf("[INFO] Inserted data in %lu cycles (%f ms) at rate of %lu cycles/element\n", t_end-t_start, (double)(t_end-t_start) * one_cycle_ns / 1000000.0, (t_end-t_start)/HT_TESTS_NUM_INSERTS);
    
// Depending on selected macro, free any allocated memory
#if defined(ZIPF)
    delete zg;
#elif defined(MINE)
    delete rand_;
#else
    clear((uint64_t*) mem);
#endif
//ZipfGen(HT_TESTS_NUM_INSERTS, theta_arg, HT_TESTS_NUM_INSERTS);

    printf(
        "[INFO] Quick stats: thread %u, Batch size: %d, cycles per "
        "insertion:%lu \n",
        sh->shard_idx, i, (t_end - t_start) / num_inserts);

#ifdef CALC_STATS
    printf(" Reprobes %lu soft_reprobes %lu\n", kmer_ht->num_reprobes,
           kmer_ht->num_soft_reprobes);
#endif
  }
  sh->stats->insertion_cycles = (t_end - t_start);
  sh->stats->num_inserts = num_inserts;
#ifndef WITH_PAPI_LIB
  get_ht_stats(sh, kmer_ht);
  // kmer_ht->display();
#endif
}

}  // namespace kmercounter
