#include "test_support.h"
#include <string.h>

static struct context_pool pool;
static struct connection clients[CONTEXT_CAPACITY + 1];

static void prepare_pool(void)
{
    memset(clients, 0, sizeof(clients));
    context_pool_init(&pool);
    for (int i = 0; i <= CONTEXT_CAPACITY; i++)
        clients[i].context_pool = &pool;
}

static void finish_pool(void)
{
    for (int i = 0; i <= CONTEXT_CAPACITY; i++)
        context_release(&clients[i]);
    CHECK(used_contexts(&pool) == 0);
    context_pool_destroy(&pool);
}

static void test_acquire_preserves_owned_data(void)
{
    /* 준비: 빈 풀과 서로 다른 연결 두 개. */
    prepare_pool();
    expect_count("initial owners", used_contexts(&pool), 0);
    /* 실행: 두 연결이 확보하고 첫 연결이 다시 확보를 요청. */
    bool first_ok = context_acquire(&clients[0]);
    bool second_ok = context_acquire(&clients[1]);
    CHECK(first_ok && second_ok);
    clients[0].context->data[0] = 0x42;
    bool repeated_ok = context_acquire(&clients[0]);
    /* 검증: 서로 다른 공간, 중복 점유 없음, 처리 중 데이터 보존. */
    CHECK(first_ok && second_ok && repeated_ok);
    CHECK(clients[0].context != clients[1].context);
    CHECK(clients[0].context->data[0] == 0x42);
    expect_count("owners after repeated acquire", used_contexts(&pool), 2);
    finish_pool();
}

static void test_full_pool_returns_failure(void)
{
    /* 준비: 모든 처리 공간을 사용 중으로 만든다. */
    prepare_pool();
    for (int i = 0; i < CONTEXT_CAPACITY; i++) {
        bool acquired = context_acquire(&clients[i]);
        CHECK(acquired);
    }
    /* 실행: 한도를 넘는 다른 연결이 확보를 요청. */
    bool extra_acquired = context_acquire(&clients[CONTEXT_CAPACITY]);
    /* 검증: 기존 소유권을 건드리지 않고 실패한다. */
    CHECK(!extra_acquired);
    CHECK(clients[CONTEXT_CAPACITY].context == NULL);
    expect_count("owners at exhaustion", used_contexts(&pool), CONTEXT_CAPACITY);
    for (int i = 0; i < CONTEXT_CAPACITY; i++) CHECK(pool.owners[i] == &clients[i]);
    finish_pool();
}

static void test_release_and_clean_reuse(void)
{
    prepare_pool();
    bool acquired = context_acquire(&clients[0]);
    CHECK(acquired);
    struct request_context *original = clients[0].context;
    memset(original, 0x7f, sizeof(*original));
    /* 실행: 반환을 두 번 시도하고 다른 연결에서 다시 확보한다. */
    context_release(&clients[0]);
    context_release(&clients[0]);
    CHECK(clients[0].context == NULL);
    expect_count("owners after release twice", used_contexts(&pool), 0);
    bool reused = context_acquire(&clients[1]);
    /* 검증: 같은 공간을 재사용하되 이전 요청 데이터는 모두 지운다. */
    CHECK(reused && clients[1].context == original);
    expect_zero_context(clients[1].context);
    expect_count("owners after reuse", used_contexts(&pool), 1);
    finish_pool();
}

static pthread_barrier_t barrier;

static void wait_for_peers(void)
{
    int status = pthread_barrier_wait(&barrier);
    CHECK(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *hold_context(void *argument)
{
    struct connection *client = argument;
    bool acquired = context_acquire(client);
    CHECK(acquired);
    wait_for_peers();
    wait_for_peers();
    context_release(client);
    return NULL;
}

static void test_concurrent_ownership(void)
{
    prepare_pool();
    pthread_t threads[CONTEXT_CAPACITY];
    check_thread(pthread_barrier_init(&barrier, NULL, CONTEXT_CAPACITY + 1));
    /* 실행: 모든 스레드가 확보를 마치고 반환 전까지 기다린다. */
    for (int i = 0; i < CONTEXT_CAPACITY; i++)
        check_thread(pthread_create(&threads[i], NULL, hold_context, &clients[i]));
    wait_for_peers();
    /* 검증: 동시 확보 중 같은 공간을 두 연결에 주지 않는다. */
    expect_count("concurrent owners", used_contexts(&pool), CONTEXT_CAPACITY);
    for (int i = 0; i < CONTEXT_CAPACITY; i++)
        for (int j = i + 1; j < CONTEXT_CAPACITY; j++)
            CHECK(clients[i].context != clients[j].context);
    wait_for_peers();
    for (int i = 0; i < CONTEXT_CAPACITY; i++) check_thread(pthread_join(threads[i], NULL));
    expect_count("owners after thread joins", used_contexts(&pool), 0);
    check_thread(pthread_barrier_destroy(&barrier));
    finish_pool();
}

static void test_fifo(void)
{
    static struct connection later;
    prepare_pool();
    memset(&later, 0, sizeof(later));
    later.context_pool = &pool;
    for (int i = 0; i < CONTEXT_CAPACITY; i++) CHECK(context_acquire(&clients[i]));
    struct connection *first = &clients[CONTEXT_CAPACITY];
    CHECK(!context_acquire(first));
    int64_t deadline = first->context_wait_deadline;
    CHECK(!context_acquire(first));
    CHECK(pool.wait_count == 1 && first->context_wait_deadline == deadline);
    CHECK(!context_acquire(&later));
    context_release(&clients[0]);
    CHECK(!context_acquire(&later)); /* 신규/후순위 추월 차단 */
    CHECK(context_acquire(first));
    context_release(first);
    CHECK(context_acquire(&later));
    context_release(&later);
    /* 종료·만료는 공통 release를 통해 선두를 제거합니다. */
    CHECK(context_acquire(&clients[0]));
    CHECK(!context_acquire(first));
    CHECK(!context_acquire(&later));
    context_release(first);
    CHECK(pool.wait_count == 1 && pool.waiters[0] == &later);
    context_release(&clients[0]);
    CHECK(context_acquire(&later));
    context_release(&later);
    finish_pool();
    CHECK(pool.wait_count == 0);
    puts("FIFO: duplicate, no overtaking, order, cancellation passed.");
}

int main(void)
{
    test_fifo();
    test_acquire_preserves_owned_data();
    test_full_pool_returns_failure();
    test_release_and_clean_reuse();
    test_concurrent_ownership();
    puts("All 4 pool unit groups passed.");
    return 0;
}
