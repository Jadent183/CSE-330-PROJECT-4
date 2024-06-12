#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/cred.h>
#include <linux/timekeeping.h>

MODULE_LICENSE("GPL");

// Module parameters
static int buffSize = 10;
static int prod = 1;
static int cons = 2;
static int uuid = 1000;

module_param(buffSize, int, 0);
MODULE_PARM_DESC(buffSize, "Buffer size");
module_param(prod, int, 0);
MODULE_PARM_DESC(prod, "Number of producers");
module_param(cons, int, 0);
MODULE_PARM_DESC(cons, "Number of consumers");
module_param(uuid, int, 0);
MODULE_PARM_DESC(uuid, "User ID");

struct task_struct **buffer;
int buffer_head = 0, buffer_tail = 0;
static struct semaphore full, empty, mutex;
static int end_flag = 0;

static int producer_function(void *data)
{
	struct task_struct *task;
	pr_info("Producer thread started\n");

	for_each_process(task)
	{
		if (task->cred->uid.val == uuid)
		{
			down(&empty);
			down(&mutex);
			buffer[buffer_head] = task;
			buffer_head = (buffer_head + 1) % buffSize;
			up(&mutex);
			up(&full);
		}
	}
	end_flag = 1;
	pr_info("Producer thread finished\n");
	return 0;
}

static int consumer_function(void *data)
{
	unsigned long total_elapsed_time = 0;
	pr_info("Consumer thread started\n");

	while (!end_flag || (down_trylock(&full) == 0))
	{
		if (end_flag && down_trylock(&full))
		{
			break;
		}
		down(&full);
		down(&mutex);
		struct task_struct *task = buffer[buffer_tail];
		buffer_tail = (buffer_tail + 1) % buffSize;
		up(&mutex);
		up(&empty);

		if (task)
		{
			struct timespec64 now, boot_time;
			ktime_get_real_ts64(&now);
			boot_time = ns_to_timespec64(task->start_time);
			total_elapsed_time += now.tv_sec - boot_time.tv_sec;
		}
	}
	pr_info("Total elapsed time: %lu\n", total_elapsed_time);
	pr_info("Consumer thread finished\n");
	return 0;
}

static struct task_struct *producer_thread;
static struct task_struct **consumer_threads;

static int __init producer_consumer_init(void)
{
	int i;

	pr_info("Initializing buffer\n");
	buffer = kmalloc_array(buffSize, sizeof(struct task_struct *), GFP_KERNEL);
	if (!buffer)
	{
		pr_err("Failed to allocate buffer\n");
		return -ENOMEM;
	}

	sema_init(&full, 0);
	sema_init(&empty, buffSize);
	sema_init(&mutex, 1);

	if (prod == 1)
	{
		pr_info("Creating producer thread\n");
		producer_thread = kthread_run(producer_function, NULL, "producer_thread");
		if (IS_ERR(producer_thread))
		{
			pr_err("Failed to create producer thread\n");
			kfree(buffer);
			return PTR_ERR(producer_thread);
		}
	}

	pr_info("Creating consumer threads\n");
	consumer_threads = kmalloc_array(cons, sizeof(struct task_struct *), GFP_KERNEL);
	if (!consumer_threads)
	{
		pr_err("Failed to allocate consumer threads\n");
		if (prod == 1)
		{
			kthread_stop(producer_thread);
		}
		kfree(buffer);
		return -ENOMEM;
	}

	for (i = 0; i < cons; i++)
	{
		consumer_threads[i] = kthread_run(consumer_function, NULL, "consumer_thread-%d", i);
		if (IS_ERR(consumer_threads[i]))
		{
			pr_err("Failed to create consumer thread %d\n", i);
			for (int j = 0; j < i; j++)
			{
				if (!IS_ERR(consumer_threads[j]))
				{
					kthread_stop(consumer_threads[j]);
				}
			}
			if (prod == 1)
			{
				kthread_stop(producer_thread);
			}
			kfree(buffer);
			kfree(consumer_threads);
			return PTR_ERR(consumer_threads[i]);
		}
	}

	pr_info("Module loaded\n");
	return 0;
}

static void __exit producer_consumer_exit(void)
{
	int i;

	pr_info("Stopping producer thread\n");
	if (prod == 1)
	{
		if (!IS_ERR(producer_thread))
		{
			kthread_stop(producer_thread);
		}
	}

	pr_info("Stopping consumer threads\n");
	for (i = 0; i < cons; i++)
	{
		if (!IS_ERR(consumer_threads[i]))
		{
			kthread_stop(consumer_threads[i]);
		}
	}

	pr_info("Freeing allocated memory\n");
	kfree(buffer);
	kfree(consumer_threads);

	pr_info("Module unloaded successfully\n");
}

module_init(producer_consumer_init);
module_exit(producer_consumer_exit);
