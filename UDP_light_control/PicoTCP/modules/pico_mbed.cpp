#include <mbed.h>
#include <rtos.h>

extern "C" {
    void *pico_mutex_init(void)
    {
        return new Mutex();
    }

    void pico_mutex_lock(void *_m)
    {
        Mutex *m = (Mutex *)_m;
        m->lock();
    }

    void pico_mutex_unlock(void *_m)
    {
        Mutex *m = (Mutex *)_m;
        m->unlock();
    }
}
