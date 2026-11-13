"""Test production guard ABI with compiler-generated local-static fast paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/libc/posix_compat.c').read_text()
start = source.index('int __cxa_guard_acquire(')
end = source.index('__attribute__((weak)) void __cxa_pure_virtual', start)
guard_source = '#include <stdint.h>\n#include <stdbool.h>\n#include <sched.h>\n'+source[start:end]
fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
extern "C" int __cxa_guard_acquire(uint64_t*);
extern "C" void __cxa_guard_release(uint64_t*);
extern "C" void __cxa_guard_abort(uint64_t*);
static std::atomic<bool> constructor_entered{false}, release_constructor{false};
static std::atomic<bool> reader_entered{false}, reader_returned{false};
static std::atomic<unsigned> constructors{0};
struct Value {
    unsigned magic;
    Value() : magic(0) {
        ++constructors;
        constructor_entered.store(true, std::memory_order_release);
        while (!release_constructor.load(std::memory_order_acquire)) sched_yield();
        magic = 0x12345678;
    }
};
static Value* instance() {
    static Value* value = new Value;
    return value;
}
static void* owner(void*) {
    assert(instance()->magic == 0x12345678);
    return nullptr;
}
static void* reader(void*) {
    reader_entered.store(true, std::memory_order_release);
    Value* value = instance();
    assert(value && value->magic == 0x12345678);
    reader_returned.store(true, std::memory_order_release);
    for (unsigned i=0;i<10000;++i) assert(instance()==value);
    return nullptr;
}
static unsigned retries = 0;
struct RetryValue {
    unsigned magic;
    RetryValue() : magic(0) {
        if (++retries == 1) throw 42;
        magic = 0x87654321;
    }
};
static RetryValue& retry_instance() {
    static RetryValue value;
    return value;
}
int main() {
    uint64_t guard = 0;
    assert(__cxa_guard_acquire(&guard)==1);
    /* 编译器快速路径会直接检查此字节，不调用 guard_acquire。 */
    assert(__atomic_load_n(reinterpret_cast<unsigned char*>(&guard),
                           __ATOMIC_ACQUIRE)==0);
    __cxa_guard_abort(&guard);
    assert(guard==0 && __cxa_guard_acquire(&guard)==1);
    __cxa_guard_release(&guard);
    assert(__atomic_load_n(reinterpret_cast<unsigned char*>(&guard),
                           __ATOMIC_ACQUIRE)==1);
    assert(__cxa_guard_acquire(&guard)==0);
    assert(__cxa_guard_acquire(nullptr)==0);
    __cxa_guard_release(nullptr);
    __cxa_guard_abort(nullptr);
    pthread_t initializer, readers[8];
    assert(pthread_create(&initializer,nullptr,owner,nullptr)==0);
    while (!constructor_entered.load(std::memory_order_acquire)) sched_yield();
    for (auto& thread:readers) assert(pthread_create(&thread,nullptr,reader,nullptr)==0);
    while (!reader_entered.load(std::memory_order_acquire)) sched_yield();
    for (unsigned i=0;i<10000;++i) sched_yield();
    assert(!reader_returned.load(std::memory_order_acquire));
    release_constructor.store(true,std::memory_order_release);
    assert(pthread_join(initializer,nullptr)==0);
    for (auto thread:readers) assert(pthread_join(thread,nullptr)==0);
    assert(constructors==1 && reader_returned);
    /* 构造函数抛出异常后，编译器必须调用 abort，使后续调用者能够重试，
     * 而不是把对象误判为已完成初始化。 */
    bool caught = false;
    try { (void)retry_instance(); }
    catch (int value) { assert(value==42); caught = true; }
    assert(caught && retries==1);
    RetryValue* recovered = &retry_instance();
    assert(recovered->magic==0x87654321 && retries==2);
    for (unsigned i=0;i<10000;++i) assert(&retry_instance()==recovered);
    assert(retries==2);
}
'''
with tempfile.TemporaryDirectory(prefix='system-cxx-guard-') as directory:
    path = Path(directory)
    (path/'guard.c').write_text(guard_source)
    (path/'test.cc').write_text(fixture)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-c',str(path/'guard.c'),'-o',str(path/'guard.o')],check=True)
    for compiler in ('g++','clang++'):
        subprocess.run([compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror',
                        '-pthread',str(path/'test.cc'),str(path/'guard.o'),
                        '-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True,timeout=15)
        print(compiler+': first-byte ABI, concurrent publication and compiler-generated exception/abort retry passed')
