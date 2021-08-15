#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/kallsyms.h>
#include <linux/tty.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <linux/smp.h>
#include <linux/miscdevice.h>
#include <linux/notifier.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "shared.h"

MODULE_LICENSE("GPL");
// Char we show before each debug print
const char program_name[] = "lkcd";

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0)
#include <linux/static_call.h>
#include <linux/kprobes.h>

static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name",
    .flags = KPROBE_FLAG_DISABLED
};

static unsigned long lkcd_lookup_name_scinit(const char *name);
unsigned long kallsyms_lookup_name_c(const char *name)
{
	return 0;
}

DEFINE_STATIC_CALL(lkcd_lookup_name_sc, lkcd_lookup_name_scinit);
#endif

// read kernel symbols from the /proc
#define KALLSYMS_PATH "/proc/kallsyms"
#define BUFF_SIZE 256

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0) && LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0)

static unsigned long lkcd_lookup_name(const char *name)
{
	unsigned int i = 0, first_space_idx = 0, second_space_idx = 0; /* Read Index and indexes of spaces */
	struct file *proc_ksyms = NULL;
	loff_t pos = 0;
	unsigned long ret = 0;
	ssize_t read = 0;
	int err = 0;
	const size_t name_len = strlen(name);

	/*
	 * Buffer for each line of kallsyms file.
	 * The symbol names are limited to KSYM_NAME_LEN=128. When Linux is
	 * compiled with clang's Control Flow Integrity, there are large symbols
	 * such as
	 * __typeid__ZTSFvPvP15ieee80211_local11set_key_cmdP21ieee80211_sub_if_dataP13ieee80211_staP18ieee80211_key_confE_global_addr
	 * which lead to a line with 142 characters.
	 * Some use a buffer which can hold 256 characters, to be safe.
	 */
	char proc_ksyms_entry[256] = {0};

	proc_ksyms = filp_open("/proc/kallsyms", O_RDONLY, 0);
	if (proc_ksyms == NULL)
		goto cleanup;

	read = kernel_read(proc_ksyms, proc_ksyms_entry + i, 1, &pos);
	while (read == 1) {
		if (proc_ksyms_entry[i] == '\n' || (size_t)i == sizeof(proc_ksyms_entry) - 1) {
			/* Prefix-match the name with the 3rd field of the line, after the second space */
			if (second_space_idx > 0 &&
			    second_space_idx + 1 + name_len <= sizeof(proc_ksyms_entry) &&
			    !strncmp(proc_ksyms_entry + second_space_idx + 1, name, name_len)) {
				printk(KERN_INFO "[+] %s: %.*s\n", name,
				       i, proc_ksyms_entry);
				/* Decode the address, which is in hexadecimal */
				proc_ksyms_entry[first_space_idx] = '\0';
				err = kstrtoul(proc_ksyms_entry, 16, &ret);
				if (err) {
					printk(KERN_ERR "kstrtoul returned error %d while parsing %.*s\n",
					       err, first_space_idx, proc_ksyms_entry);
					ret = 0;
					goto cleanup;
				}
				goto cleanup;
			}

			i = 0;
			first_space_idx = 0;
			second_space_idx = 0;
			memset(proc_ksyms_entry, 0, sizeof(proc_ksyms_entry));
		} else {
			if (proc_ksyms_entry[i] == ' ') {
				if (first_space_idx == 0) {
					first_space_idx = i;
				} else if (second_space_idx == 0) {
					second_space_idx = i;
				}
			}
			i++;
		}
		read = kernel_read(proc_ksyms, proc_ksyms_entry + i, 1, &pos);
	}
	printk(KERN_ERR "symbol not found in kallsyms: %s\n", name);

cleanup:
	if (proc_ksyms != NULL)
		filp_close(proc_ksyms, 0);
	return ret;
}

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0)

static struct kprobe kp = {
	.symbol_name = "kallsyms_lookup_name",
	.flags = KPROBE_FLAG_DISABLED
};

static unsigned long lkcd_lookup_name_scinit(const char *name)
{
	unsigned long (*lkcd_lookup_name_fp)(const char *name) = NULL;
	int kp_ret;

	// try kprobes first, but have a fallback as they might be disabled
	kp_ret = register_kprobe(&kp);
	if (kp_ret < 0) {
		dbgprint("register_kprobe failed, returned %d", kp_ret);
	} else {
		lkcd_lookup_name_fp = (unsigned long (*) (const char *name))kp.addr;
		unregister_kprobe(&kp);
	}

	// brute force by doing a symbolic search via sprint_symbol
	if (!lkcd_lookup_name_fp) {
		char name[KSYM_SYMBOL_LEN];
		unsigned long start = (unsigned long) sprint_symbol;
		unsigned long end = start - 32 * 1024;
		unsigned long addr, offset;
		char *off_ptr;

		for (addr = start; addr > end; addr--) {
			if (sprint_symbol(name, addr) <= 0)
				break;
			if (!strncmp(name, "0x", 2))
				break;
			off_ptr = strchr(name, '+');
			if (!off_ptr)
				break;
			if (sscanf(off_ptr, "+%lx", &offset) != 1)
				break;
			addr -= offset;
			if (off_ptr - name == 20 &&
			    !strncmp(name, "kallsyms_lookup_name", 20))
			{
				lkcd_lookup_name_fp = (void *)addr;
				break;
			}
		}

		if (!lkcd_lookup_name_fp)
			dbgprint("lookup via sprint_symbol() failed, too");
	}

	if (lkcd_lookup_name_fp) {
		static_call_update(lkcd_lookup_name_sc, lkcd_lookup_name_fp);
		return static_call(lkcd_lookup_name_sc)(name);
	}

	return 0;
}

static unsigned long lkcd_lookup_name(const char *name)
{
 return static_call(lkcd_lookup_name_sc)(name);
}

#else
unsigned long lkcd_lookup_name(const char *name)
{
 return kallsyms_lookup_name(name);
}

#endif

static int open_lkcd(struct inode *inode, struct file *file)
{
  try_module_get(THIS_MODULE);
  return 0;
}

static int close_lkcd(struct inode *inode, struct file *file) 
{ 
  module_put(THIS_MODULE);  
  return 0;
} 

static long lkcd_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
//  int numargs = 0;
  unsigned long ptrbuf[16];
//  unsigned long *ptr = ptrbuf;
  switch(ioctl_num)
  {
    case IOCTL_RKSYM:
     {
       char name[BUFF_SIZE];
       int i;
       char ch;
       char *temp = (char *) ioctl_param;
       get_user(ch, temp++);
       name[0] = ch;
       for (i = 1; ch && i < BUFF_SIZE - 1; i++, temp++) 
       {
          get_user(ch, temp);
          name[i] = ch;
       }
       ptrbuf[0] = lkcd_lookup_name(name);
       if (copy_to_user((void*)ioctl_param, (void*)ptrbuf, sizeof(ptrbuf[0])) > 0)
 	 return -EFAULT;
      }
      break; /* IOCTL_RKSYM */

    case IOCTL_CNTNTFYCHAIN:
     {
       // copy address of blocking_notifier_head from user-mode
       struct blocking_notifier_head *nb;
       struct notifier_block *b;
       unsigned long res = 0;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long)) > 0 )
 	 return -EFAULT;
       nb = (struct blocking_notifier_head *)ptrbuf[0];
       // lock
       down_write(&nb->rwsem);
       // traverse
       if ( nb->head != NULL )
       {
          for ( b = nb->head; b != NULL; b = b->next )
            res++;
       }
       // unlock
       up_write(&nb->rwsem);
       // copy count to user-mode
       if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
 	 return -EFAULT;
     }
     break; /* IOCTL_CNTNTFYCHAIN */

    case IOCTL_ENUMNTFYCHAIN:
     {
       // copy address of blocking_notifier_head and count from user-mode
       struct blocking_notifier_head *nb;
       unsigned long cnt;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long) * 2) > 0 )
 	 return -EFAULT;
       nb = (struct blocking_notifier_head *)ptrbuf[0];
       cnt = ptrbuf[1];
       // validation
       if ( !cnt || !nb )
         return EINVAL;
       else
       {
         struct notifier_block *b;
         unsigned long res = 0; // how many ntfy in reality
         unsigned long *kbuf = (unsigned long *)kmalloc_array(cnt, sizeof(unsigned long), GFP_KERNEL);
         if ( !kbuf )
           return -ENOMEM;
         // lock
         down_write(&nb->rwsem);
         // traverse
         if ( nb->head != NULL )
         {
            for ( b = nb->head; (b != NULL) && (res < cnt); b = b->next )
            {
              kbuf[res] = (unsigned long)b->notifier_call;
              res++;
            }
         }
         // unlock
         up_write(&nb->rwsem);
         // copy count to user-mode
         if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
         {
           kfree(kbuf);
           return -EFAULT;
         }
         if ( res )
         {
           if ( copy_to_user((void*)(ioctl_param + sizeof(res)), (void*)kbuf, sizeof(unsigned long) * res) > 0 )
           {
             kfree(kbuf);
             return -EFAULT;
           }
         }
         // cleanup
         kfree(kbuf);
       }
     }
     break; /* IOCTL_ENUMNTFYCHAIN */

    case IOCTL_ENUMANTFYCHAIN:
     {
       // copy address of atomic_notifier_head and count from user-mode
       struct atomic_notifier_head *nb;
       unsigned long cnt;
       unsigned long flags;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long) * 2) > 0 )
 	 return -EFAULT;
       nb = (struct atomic_notifier_head *)ptrbuf[0];
       cnt = ptrbuf[1];
       // validation
       if ( !cnt || !nb )
         return EINVAL;
       else
       {
         struct notifier_block *b;
         unsigned long res = 0; // how many ntfy in reality
         unsigned long *kbuf = (unsigned long *)kmalloc_array(cnt, sizeof(unsigned long), GFP_KERNEL);
         if ( !kbuf )
           return -ENOMEM;
         // lock
         spin_lock_irqsave(&nb->lock, flags);
         // traverse
         if ( nb->head != NULL )
         {
            for ( b = nb->head; (b != NULL) && (res < cnt); b = b->next )
            {
              kbuf[res] = (unsigned long)b->notifier_call;
              res++;
            }
         }
         // unlock
         spin_unlock_irqrestore(&nb->lock, flags);
         // copy count to user-mode
         if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
         {
           kfree(kbuf);
           return -EFAULT;
         }
         if ( res )
         {
           if ( copy_to_user((void*)(ioctl_param + sizeof(res)), (void*)kbuf, sizeof(unsigned long) * res) > 0 )
           {
             kfree(kbuf);
             return -EFAULT;
           }
         }
         // cleanup
         kfree(kbuf);
       }
     }
     break; /* IOCTL_ENUMANTFYCHAIN */

    case IOCTL_CNTANTFYCHAIN:
     {
       // copy address of atomic_notifier_head from user-mode
       struct atomic_notifier_head *nb;
       struct notifier_block *b;
       unsigned long flags;
       unsigned long res = 0;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long)) > 0 )
 	 return -EFAULT;
       nb = (struct atomic_notifier_head *)ptrbuf[0];
       // lock
       spin_lock_irqsave(&nb->lock, flags);
       // traverse
       if ( nb->head != NULL )
       {
          for ( b = nb->head; b != NULL; b = b->next )
            res++;
       }
       // unlock
       spin_unlock_irqrestore(&nb->lock, flags);
       // copy count to user-mode
       if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
 	 return -EFAULT;
     }
     break; /* IOCTL_CNTANTFYCHAIN */

    case IOCTL_ENUMSNTFYCHAIN:
     {
       // copy address of srcu_notifier_head and count from user-mode
       struct srcu_notifier_head *nb;
       unsigned long cnt;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long) * 2) > 0 )
 	 return -EFAULT;
       nb = (struct srcu_notifier_head *)ptrbuf[0];
       cnt = ptrbuf[1];
       // validation
       if ( !cnt || !nb )
         return EINVAL;
       else
       {
         struct notifier_block *b;
         unsigned long res = 0; // how many ntfy in reality
         unsigned long *kbuf = (unsigned long *)kmalloc_array(cnt, sizeof(unsigned long), GFP_KERNEL);
         if ( !kbuf )
           return -ENOMEM;
         // lock
         mutex_lock(&nb->mutex);
         // traverse
         if ( nb->head != NULL )
         {
            for ( b = nb->head; (b != NULL) && (res < cnt); b = b->next )
            {
              kbuf[res] = (unsigned long)b->notifier_call;
              res++;
            }
         }
         // unlock
         mutex_unlock(&nb->mutex);
         // copy count to user-mode
         if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
         {
           kfree(kbuf);
           return -EFAULT;
         }
         if ( res )
         {
           if ( copy_to_user((void*)(ioctl_param + sizeof(res)), (void*)kbuf, sizeof(unsigned long) * res) > 0 )
           {
             kfree(kbuf);
             return -EFAULT;
           }
         }
         // cleanup
         kfree(kbuf);
       }
     }
     break; /* IOCTL_ENUMSNTFYCHAIN */

    case IOCTL_CNTSNTFYCHAIN:
     {
       // copy address of srcu_notifier_head from user-mode
       struct srcu_notifier_head *nb;
       struct notifier_block *b;
       unsigned long res = 0;
       if ( copy_from_user( (void*)ptrbuf, (void*)ioctl_param, sizeof(long)) > 0 )
 	 return -EFAULT;
       nb = (struct srcu_notifier_head *)ptrbuf[0];
       // lock
       mutex_lock(&nb->mutex);
       // traverse
       if ( nb->head != NULL )
       {
          for ( b = nb->head; b != NULL; b = b->next )
            res++;
       }
       // unlock
       mutex_unlock(&nb->mutex);
       // copy count to user-mode
       if ( copy_to_user((void*)ioctl_param, (void*)&res, sizeof(res)) > 0 )
 	 return -EFAULT;
     }
     break; /* IOCTL_CNTSNTFYCHAIN */

    default:
     return -EINVAL;
  }
  return 0;
}

static loff_t memory_lseek(struct file *file, loff_t offset, int orig)
{
	loff_t ret;

	inode_lock(file_inode(file));
	switch (orig) {
	case SEEK_CUR:
		offset += file->f_pos;
		/* fall through */
	case SEEK_SET:
		/* to avoid userland mistaking f_pos=-9 as -EBADF=-9 */
		if ((unsigned long long)offset >= -MAX_ERRNO) {
			ret = -EOVERFLOW;
			break;
		}
		file->f_pos = offset;
		ret = file->f_pos;
		force_successful_syscall_return();
		break;
	default:
		ret = -EINVAL;
	}
	inode_unlock(file_inode(file));
	return ret;
}

static inline unsigned long size_inside_page(unsigned long start,
					     unsigned long size)
{
	unsigned long sz;

	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}

static inline bool should_stop_iteration(void)
{
	if (need_resched())
		cond_resched();
	return fatal_signal_pending(current);
}

static ssize_t read_kmem(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t low_count, read, sz;
	char *kbuf; /* k-addr because vread() takes vmlist_lock rwlock */
	int err = 0;

	read = 0;
	if (p < (unsigned long) high_memory) {
		low_count = count;
		if (count > (unsigned long)high_memory - p)
			low_count = (unsigned long)high_memory - p;

#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
		/* we don't have page 0 mapped on sparc and m68k.. */
		if (p < PAGE_SIZE && low_count > 0) {
			sz = size_inside_page(p, low_count);
			if (clear_user(buf, sz))
				return -EFAULT;
			buf += sz;
			p += sz;
			read += sz;
			low_count -= sz;
			count -= sz;
		}
#endif
		while (low_count > 0) {
			sz = size_inside_page(p, low_count);

			/*
			 * On ia64 if a page has been mapped somewhere as
			 * uncached, then it must also be accessed uncached
			 * by the kernel or data corruption may occur
			 */
			kbuf = xlate_dev_kmem_ptr((void *)p);
			if (!virt_addr_valid(kbuf))
				return -ENXIO;

			if (copy_to_user(buf, kbuf, sz))
				return -EFAULT;
			buf += sz;
			p += sz;
			read += sz;
			low_count -= sz;
			count -= sz;
			if (should_stop_iteration()) {
				count = 0;
				break;
			}
		}
	}

	if (count > 0) {
		if (copy_to_user(buf, (const void *)p, count)) {
			err = -EFAULT;
		} else {
                	p += count;
                	read += count;
		}
	}
	*ppos = p;
	return read ? read : err;
}

static const struct file_operations kmem_fops = {
	.llseek		= memory_lseek,
	.read		= read_kmem,
	.open		= open_lkcd,
	.release        = close_lkcd,
	.unlocked_ioctl	= lkcd_ioctl,
};

static struct miscdevice lkcd_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "lkcd",
    .fops = &kmem_fops
};

int __init
init_module (void)
{
  int ret = misc_register(&lkcd_dev);
  if (ret)
  {
    printk("Unable to register the lkcd device\n");
    return ret;
  }
  return 0;
}

void cleanup_module (void)
{
  misc_deregister(&lkcd_dev);
}
