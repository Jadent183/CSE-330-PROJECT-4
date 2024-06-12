#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

MODULE_LICENSE("GPL");

// Module parameters
static int buffSize = 10;
static int prod = 1;
static int cons = 1;
static int uuid = 1000;

module_param(buffSize, int, 0);
MODULE_PARM_DESC(buffSize, "Buffer size");
module_param(prod, int, 0);
MODULE_PARM_DESC(prod, "Number of producers");
module_param(cons, int, 0);
MODULE_PARM_DESC(cons, "Number of consumers");
module_param(uuid, int, 0);
MODULE_PARM_DESC(uuid, "User ID");

// Buffer and synchronization primitives
struct process_info
{
	unsigned long pid;
	unsigned long long start_time;
	unsigned long long boot_time;
};

static struct process_info *buffer;
static int fill = 0, use = 0;
static struct semaphore empty, full, mutex;
static int end_flag = 0;

// Threads
static struct task_struct *producer_thread;
static struct task_struct **consumer_threads;

static unsigned long long total_time_elapsed = 0;
static int total_no_of_process_produced = 0;
static int total_no_of_process_consumed = 0;

// Producer function
static int producer_function(void *data)
{
	struct task_struct *task;
	allow_signal(SIGKILL);

	for_each_process(task)
	{
		if (task->cred->uid.val == uuid)
		{
			if (kthread_should_stop())
				break;

			down_interruptible(&empty);
			down_interruptible(&mutex);

			buffer[fill].pid = task->pid;
			buffer[fill].start_time = task->start_time;
			buffer[fill].boot_time = ktime_get_boottime_ns();
			fill = (fill + 1) % buffSize;

			up(&mutex);
			up(&full);

			total_no_of_process_produced++;
			pr_info("[%s] Produce-Item#:%d at buffer index: %d for PID:%lu\n", current->comm,
					total_no_of_process_produced, (fill + buffSize - 1) % buffSize, task->pid);
		}
	}

	end_flag = 1;
	pr_info("[%s] Producer Thread stopped.\n", current->comm);
	return 0;
}

// Consumer function
static int consumer_function(void *data)
{
	int no_of_process_consumed = 0;
	struct process_info process;
	unsigned long long ktime, process_time_elapsed;
	allow_signal(SIGKILL);

	while (!kthread_should_stop())
	{
		if (end_flag && down_trylock(&full))
		{
			break;
		}

		down_interruptible(&full);
		down_interruptible(&mutex);

		process = buffer[use];
		use = (use + 1) % buffSize;

		up(&mutex);
		up(&empty);

		if (process.pid != 0)
		{
			ktime = ktime_get_ns();
			process_time_elapsed = (ktime - process.start_time) / 1000000000;
			total_time_elapsed += ktime - process.start_time;

			no_of_process_consumed++;
			total_no_of_process_consumed++;
			pr_info("[%s] Consumed Item#-%d on buffer index:%d::PID:%lu \t Elapsed Time %llu\n", current->comm,
					no_of_process_consumed, (use + buffSize - 1) % buffSize, process.pid, process_time_elapsed);
		}
	}

	pr_info("[%s] Consumer Thread stopped.\n", current->comm);
	return 0;
}

static int __init producer_consumer_init(void)
{
	int i;

	pr_info("Kernel module received the following inputs: UID:%d, Buffer-Size:%d, No of Producer:%d, No of Consumer:%d\n",
			uuid, buffSize, prod, cons);

	if (buffSize <= 0 || prod < 0 || prod > 1 || cons < 0)
	{
		pr_err("Invalid parameters\n");
		return -EINVAL;
	}

	buffer = kmalloc_array(buffSize, sizeof(struct process_info), GFP_KERNEL);
	if (!buffer)
	{
		pr_err("Failed to allocate buffer\n");
		return -ENOMEM;
	}

	sema_init(&empty, buffSize);
	sema_init(&full, 0);
	sema_init(&mutex, 1);

	producer_thread = kthread_run(producer_function, NULL, "producer_thread");
	if (IS_ERR(producer_thread))
	{
		pr_err("Failed to create producer thread\n");
		kfree(buffer);
		return PTR_ERR(producer_thread);
	}

	consumer_threads = kmalloc_array(cons, sizeof(struct task_struct *), GFP_KERNEL);
	if (!consumer_threads)
	{
		pr_err("Failed to allocate consumer threads\n");
		kthread_stop(producer_thread);
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
				kthread_stop(consumer_threads[j]);
			}
			kthread_stop(producer_thread);
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
	if (!IS_ERR(producer_thread))
	{
		kthread_stop(producer_thread);
	}

	pr_info("Stopping consumer threads\n");
	for (i = 0; i < cons; i++)
	{
		if (!IS_ERR(consumer_threads[i]))
		{
			kthread_stop(consumer_threads[i]);
		}
	}

	kfree(buffer);
	kfree(consumer_threads);

	pr_info("Total number of items produced: %d\n", total_no_of_process_produced);
	pr_info("Total number of items consumed: %d\n", total_no_of_process_consumed);
	pr_info("Total elapsed time: %llu seconds\n", total_time_elapsed / 1000000000);
	pr_info("Module unloaded\n");
}

module_init(producer_consumer_init);
module_exit(producer_consumer_exit);
