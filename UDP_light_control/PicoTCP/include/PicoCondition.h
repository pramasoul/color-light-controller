#ifndef __PICOMUTEX__
#define __PICOMUTEX__
/*
* Cross-Threading Mutex Class
*/

#include "mbed.h"
#include "rtos.h"
#include "Queue.h"

class PicoCondition
{
    private:
        Queue <int,1> * queue;
    public:
        PicoCondition()
        {
            queue = new Queue<int,1>();
        }
        
        ~PicoCondition()
        {
            if(queue)
            {
                delete queue;
                queue = NULL;
            }
        }
        
        bool unlock(uint32_t millisec=0,int * ptr=NULL)
        {
            osStatus status;
            status = queue->put(ptr, millisec);
            return (status == osEventMessage || status == osOK);
        }
        
        bool lock(uint32_t millisec=osWaitForever)
        {
            osEvent event = queue->get(millisec);
            return (event.status == osEventMessage || event.status == osOK);
        }
};


#endif