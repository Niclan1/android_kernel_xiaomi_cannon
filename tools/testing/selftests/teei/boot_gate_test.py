#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual TEE init/gate functions with a pthread test backend.

This checks ordering/error paths only. It does not emulate secure firmware.
Run from the kernel tree; generated files stay in --output-dir.
"""
from pathlib import Path
import argparse
import hashlib
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=Path, default=Path.cwd() / '.teei-boot-tests')
args = parser.parse_args()
K = Path(__file__).resolve().parents[4]
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
src = (K / 'drivers/tee/teei/400/tz_driver/teei_client_main.c').read_text()
core = (K / 'drivers/tee/teei/400/tee/tee_core.c').read_text()
loader = (K / 'drivers/tee/teei/400/tz_driver/sysfs.c').read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end = text.index('\n}', opening) + 2
    return text[start:end] + '\n'

# Verify the device-only entry point owns the gate, never the in-kernel loader.
assert 'teei_wait_for_init()' in function(core, 'static int tee_open(')
assert 'teei_wait_for_init()' not in function(core, 'int tee_k_open(')
assert 'mutex_lock' not in function(src, 'static long teei_config_ioctl(').split('case TEEI_CONFIG_IOCTL_UNLOCK:')[1]
functions = '\n'.join(function(src, s) for s in (
    'int is_teei_ready(', 'int teei_framework_ready(', 'int teei_wait_for_init(',
    'static int teei_config_init_once(', 'static long teei_config_ioctl(')
)
functions += '\n'.join(function(loader, s) for s in (
    'static int open_driver_session(', 'static int get_driver_id(')
)
backend = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define HZ 100
#define BIT(n) (1U << (n))
#define __user
#define READ_ONCE(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
static char preload_summary[256];
static void test_log(const char *fmt, ...)
{
    if (!strstr(fmt, "boot preloads attempted=")) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(preload_summary, sizeof(preload_summary), fmt, ap);
    va_end(ap);
}
#define pr_err(...) test_log(__VA_ARGS__)
#define pr_warn(...) test_log(__VA_ARGS__)
#define IMSG_WARN(...) test_log(__VA_ARGS__)
#define IMSG_ERROR(...) test_log(__VA_ARGS__)
#define pr_info(...) test_log(__VA_ARGS__)
#define pr_info_ratelimited(...) test_log(__VA_ARGS__)
#define pr_err_ratelimited(...) test_log(__VA_ARGS__)
#define TEEI_BOOT_FOOTPRINT(s) ((void)(s))
#define TEEI_BOOT_OK 0
#define MAX_DRV_UUIDS 30
#define UUID_LEN 32
#define TEEI_CONFIG_IOCTL_INIT_TEEI 3
#define TEEI_CONFIG_IOCTL_UNLOCK 4
struct file { int unused; };
struct init_param { char uuids[30][33]; uint32_t uuid_count, flag; };
struct task { char comm[16]; } task = { "host-test" };
#define current (&task)
static int task_pid_nr(struct task *t) { (void)t; return 0; }
struct completion { pthread_mutex_t lock; pthread_cond_t cv; bool done; };
#define DECLARE_COMPLETION(n) struct completion n = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false }
#define DEFINE_MUTEX(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
static DECLARE_COMPLETION(teei_init_done);
static DECLARE_COMPLETION(boot_decryto_lock);
static DEFINE_MUTEX(teei_init_mutex);
static int teei_init_result = -ENODEV;
static bool teei_init_started, teei_transport_ready;
static unsigned int teei_flags;
static unsigned long teei_config_flag;
static int injected_wait;
static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_cv = PTHREAD_COND_INITIALIZER;
static int hold_load_at;
static int load_results[MAX_DRV_UUIDS];
static char attempted[MAX_DRV_UUIDS][UUID_LEN + 1];
static int framework_calls, load_calls, framework_error;
static bool client_finished;
int is_teei_ready(void);
int teei_framework_ready(void);
static int mutex_lock_interruptible(pthread_mutex_t *p) { return -pthread_mutex_lock(p); }
#define mutex_unlock(p) assert(pthread_mutex_unlock(p) == 0)
static void complete_all(struct completion *c)
{
    assert(pthread_mutex_lock(&c->lock) == 0);
    c->done = true;
    assert(pthread_cond_broadcast(&c->cv) == 0);
    assert(pthread_mutex_unlock(&c->lock) == 0);
}
#define complete(c) complete_all(c)
static long wait_for_completion_interruptible_timeout(struct completion *c, long timeout)
{
    (void)timeout;
    int injected = READ_ONCE(injected_wait);
    if (injected) return injected == 1 ? 0 : injected;
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_mutex_lock(&c->lock) == 0);
    int ret = 0;
    while (!c->done && !ret) ret = pthread_cond_timedwait(&c->cv, &c->lock, &deadline);
    bool done = c->done;
    assert(pthread_mutex_unlock(&c->lock) == 0);
    assert(ret == 0 || ret == ETIMEDOUT);
    return done ? 1 : 0;
}
static int copy_from_user(void *to, const void *from, size_t n) { memcpy(to, from, n); return 0; }
#define copy_to_user copy_from_user
static const char *teei_boot_error_to_string(int error) { (void)error; return "host"; }
static int init_teei_framework(void)
{
    assert(pthread_mutex_lock(&control_lock) == 0);
    ++framework_calls;
    assert(pthread_cond_broadcast(&control_cv) == 0);
    int error = framework_error;
    assert(pthread_mutex_unlock(&control_lock) == 0);
    if (error) return error;
    assert(wait_for_completion_interruptible_timeout(&boot_decryto_lock, 60 * HZ) > 0);
    return 0;
}
static int tz_load_ta_by_str(const char *uuid)
{
    assert(uuid && teei_framework_ready() && !is_teei_ready());
    assert(pthread_mutex_lock(&control_lock) == 0);
    int count = ++load_calls;
    assert(pthread_cond_broadcast(&control_cv) == 0);
    assert(count <= MAX_DRV_UUIDS);
    memcpy(attempted[count - 1], uuid, UUID_LEN + 1);
    while (hold_load_at == count) assert(pthread_cond_wait(&control_cv, &control_lock) == 0);
    int ret = load_results[count - 1];
    assert(pthread_mutex_unlock(&control_lock) == 0);
    return ret;
}
#define tz_load_drv_by_str tz_load_ta_by_str

#define TEEC_SUCCESS 0U
#define TEEC_ERROR_ITEM_NOT_FOUND 0xffff0008U
#define TEEC_ERROR_SECURITY 0xffff000fU
#define TEEC_ERROR_COMMUNICATION 0xffff000eU
#define TEEC_LOGIN_PUBLIC 0
#define UT_DRV_GET_DRIVER_ID 1
struct TEEC_UUID { unsigned int timeLow; };
struct TEEC_Session { int unused; };
struct TEEC_Operation { int unused; };
struct ut_drv_param { int cmd_id; };
struct ut_drv_entry { struct TEEC_UUID uuid; struct TEEC_Session session; unsigned int driver_id; };
static int ut_drv_context, get_result_calls, open_calls;
static unsigned int secure_result;
static unsigned int TEEC_OpenSession(void *ctx, struct TEEC_Session *session,
    struct TEEC_UUID *uuid, int login, void *conn, void *op, unsigned int *origin)
{
    assert(ctx == &ut_drv_context && session && uuid && login == TEEC_LOGIN_PUBLIC);
    assert(!conn && !op && origin);
    ++open_calls;
    *origin = 3;
    return secure_result;
}
static void prepare_params(void *op, void *param, unsigned int size)
{
    assert(op && param && size == sizeof(struct ut_drv_param));
}
static unsigned int TEEC_InvokeCommand(void *session, int command, void *op, void *origin)
{
    assert(session && command == UT_DRV_GET_DRIVER_ID && op && !origin);
    return secure_result;
}
static int get_result(void *op) { assert(op); ++get_result_calls; return 0; }

'''
tests = r'''
struct call { struct init_param param; int ret; };
static struct call make_call(void)
{
    struct call c = {0};
    /* Stock daemon list, including its automatically appended SPI driver. */
    static const char uuids[][UUID_LEN + 1] = {
        "020b0000000000000000000000000000",
        "020f0000000000000000000000000000",
        "06090000000000000000000000000000",
        "05120000000000000000000000000000",
        "40188311faf343488db888ad39496f9a",
        "07060000000000000000000000007169",
        "4be4f7dc1f2c11e5b5f7727283247c7f",
        "9073f03a9618383bb1856eb3f990babd",
        "08050000000000000000000000003419",
        "5020170115e016302017012521300000",
        "0f5eed3c3b5a47afacca69a84bf0efad",
        "07407000000000000000000000000000",
        "05160000000000000000000000000000",
        "91dba524a6e24909acc48abd7163121a",
        "57f02fe81d5a11ea8d23d7abe98b8462",
        "08070000000000000000000000008270",
        "09080000000000000000000000009381",
        "08030000000000000000000000000000",
        "85f630e0f0964c5fa2cc268ce04e3da3",
        "98fb95bcb4bf42d26473eae48690d7ea",
        "7b66512021214487ba710a51d7ea78fe",
        "09010000000000000000000000000000",
        "93feffccd8ca11e796c7c7a21acb4932",
    };
    c.param.uuid_count = sizeof(uuids) / sizeof(uuids[0]);
    c.param.flag = BIT(17) | BIT(18) | BIT(19) | BIT(20) | BIT(21);
    memcpy(c.param.uuids, uuids, sizeof(uuids));
    return c;
}
static void *initializer(void *arg)
{
    struct call *c = arg;
    c->ret = teei_config_ioctl(NULL, TEEI_CONFIG_IOCTL_INIT_TEEI, (unsigned long)&c->param);
    return NULL;
}
static void *client(void *arg)
{
    int *ret = arg;
    *ret = teei_wait_for_init();
    WRITE_ONCE(client_finished, true);
    return NULL;
}
static void unlock_daemon(void)
{
    assert(teei_config_ioctl(NULL, TEEI_CONFIG_IOCTL_UNLOCK, 0) == 0);
}
static void await_counter(int *counter, int value)
{
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 5;
    assert(pthread_mutex_lock(&control_lock) == 0);
    while (*counter < value)
        assert(pthread_cond_timedwait(&control_cv, &control_lock, &until) == 0);
    assert(pthread_mutex_unlock(&control_lock) == 0);
}
static void success_race(void)
{
    struct call a = make_call(), b = make_call();
    int gp_ret = -999;
    pthread_t gp, first, second;
    hold_load_at = 23;
    assert(pthread_create(&gp, NULL, client, &gp_ret) == 0);
    assert(pthread_create(&first, NULL, initializer, &a) == 0);
    await_counter(&framework_calls, 1);
    assert(!is_teei_ready() && !teei_framework_ready());
    assert(pthread_create(&second, NULL, initializer, &b) == 0);
    /* This ioctl must complete while INIT holds its own mutex. */
    unlock_daemon();
    await_counter(&load_calls, 23);
    assert(teei_framework_ready() && !is_teei_ready());
    assert(!READ_ONCE(client_finished));
    assert(pthread_mutex_lock(&control_lock) == 0);
    hold_load_at = 0;
    assert(pthread_cond_broadcast(&control_cv) == 0);
    assert(pthread_mutex_unlock(&control_lock) == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(pthread_join(gp, NULL) == 0);
    assert(a.ret == 0 && b.ret == 0 && gp_ret == 0);
    assert(a.param.flag == 1 && b.param.flag == 1);
    assert(framework_calls == 1 && load_calls == 23 && is_teei_ready());
    unsigned int loaded = 0, missing = 0, failed = 0;
    for (unsigned int i = 0; i < 23; ++i) {
        if (!load_results[i]) ++loaded;
        else if (load_results[i] == -ENOENT) ++missing;
        else ++failed;
    }
    char expected[256];
    snprintf(expected, sizeof(expected),
        "teei: boot preloads attempted=23 loaded=%u missing=%u failed=%u\n",
        loaded, missing, failed);
    assert(!strcmp(preload_summary, expected));
    assert(teei_config_flag == 1);
    assert(!memcmp(attempted, make_call().param.uuids, 23 * (UUID_LEN + 1)));
    a = make_call();
    initializer(&a);
    assert(a.ret == 0 && a.param.flag == 1 && framework_calls == 1 && load_calls == 23);
}
static void framework_failure(void)
{
    struct call a = make_call();
    framework_error = 5;
    unlock_daemon();
    initializer(&a);
    assert(a.ret == -EIO && a.param.flag == 0);
    assert(teei_wait_for_init() == -EIO && !is_teei_ready() && teei_config_flag == 0);
    assert(framework_calls == 1 && load_calls == 0);
    a = make_call();
    initializer(&a);
    assert(a.ret == -EIO && a.param.flag == 0 && framework_calls == 1);
}
static void loader_results(void)
{
    struct ut_drv_entry entry = {0};
    assert(open_driver_session(&entry) == 0 && open_calls == 1);
    secure_result = TEEC_ERROR_ITEM_NOT_FOUND;
    assert(open_driver_session(&entry) == -ENOENT && open_calls == 2);
    secure_result = TEEC_ERROR_SECURITY;
    assert(open_driver_session(&entry) == -EIO && open_calls == 3);
    secure_result = TEEC_ERROR_COMMUNICATION;
    assert(open_driver_session(&entry) == -EIO && open_calls == 4);
    /* A failed Invoke with an all-zero result must not cache a loaded driver. */
    assert(get_driver_id(&entry) == -EIO);
    secure_result = TEEC_SUCCESS;
    assert(get_driver_id(&entry) == 0);
    assert(get_result_calls == 2);
}
static void invalid_requests(void)
{
    struct call a = make_call();
    a.param.uuid_count = MAX_DRV_UUIDS + 1;
    initializer(&a);
    assert(a.ret == -EINVAL && !teei_init_started);
    a = make_call();
    memset(a.param.uuids[0], '1', sizeof(a.param.uuids[0]));
    initializer(&a);
    assert(a.ret == -EINVAL && !teei_init_started);
    a = make_call();
    a.param.uuids[0][31] = 0;
    initializer(&a);
    assert(a.ret == -EINVAL && !teei_init_started && framework_calls == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "success")) success_race();
    else if (!strcmp(argv[1], "framework-failure")) framework_failure();
    else if (!strcmp(argv[1], "first-missing")) { load_results[0] = -ENOENT; success_race(); }
    else if (!strcmp(argv[1], "first-eio")) { load_results[0] = -EIO; success_race(); }
    else if (!strcmp(argv[1], "last-missing")) { load_results[22] = -ENOENT; success_race(); }
    else if (!strcmp(argv[1], "mixed-optional-errors")) {
        load_results[0] = -ENOENT; load_results[5] = -EIO;
        load_results[18] = -EACCES; load_results[22] = -ENOENT;
        success_race();
    }
    else if (!strcmp(argv[1], "loader-results")) loader_results();
    else if (!strcmp(argv[1], "invalid")) invalid_requests();
    else if (!strcmp(argv[1], "timeout")) { injected_wait = 1; assert(teei_wait_for_init() == -ETIMEDOUT && !is_teei_ready()); }
    else if (!strcmp(argv[1], "interrupted")) { injected_wait = -EINTR; assert(teei_wait_for_init() == -EINTR && !is_teei_ready()); }
    else abort();
    printf("PASS %s\n", argv[1]);
    return 0;
}
'''
source = out / 'teei-host-regression.c'
exe = out / 'teei-host-regression'
source.write_text(backend + '\n' + functions + '\n' + tests)
subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined', '-pthread',
                str(source), '-o', str(exe)], check=True)
for case in ('success', 'first-missing', 'first-eio', 'last-missing',
             'mixed-optional-errors', 'framework-failure', 'loader-results',
             'invalid', 'timeout', 'interrupted'):
    subprocess.run([str(exe), case], check=True, timeout=15)
print('source SHA256:', hashlib.sha256(src.encode()).hexdigest())
print('loader SHA256:', hashlib.sha256(loader.encode()).hexdigest())
print('PASS complete stock preload pass, late GP release, one-shot INIT, framework failure')