/*
    i2c-proc.c - A Linux module for reading sensor data.
    Copyright (c) 1998  Frodo Looijaard <frodol@dds.nl> 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <linux/proc_fs.h>

#include "i2c.h"
#include "smbus.h"
#include "isa.h"
#include "version.h"
#include "compat.h"
#include "sensors.h"

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

static int i2cproc_init(void);
static int i2cproc_cleanup(void);
static int i2cproc_attach_adapter(struct i2c_adapter *adapter);
static int i2cproc_detach_client(struct i2c_client *client);
static int i2cproc_command(struct i2c_client *client, unsigned int cmd,
                           void *arg);
static void i2cproc_inc_use(struct i2c_client *client);
static void i2cproc_dec_use(struct i2c_client *client);

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0))

static int read_bus_i2c(char *buf, char **start, off_t offset, int len,
                        int *eof , void *private);

static struct proc_dir_entry *proc_bus_i2c;

#else /* (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0)) */

static int read_bus_i2c(char *buf, char **start, off_t offset, int len,
                        int unused);

static struct proc_dir_entry proc_bus_dir =
  {
    /* low_ino */	0,     /* Set by proc_register_dynamic */
    /* namelen */	3, 
    /* name */		"bus",
    /* mode */		S_IRUGO | S_IXUGO | S_IFDIR,
    /* nlink */		1,     /* Corrected by proc_register[_dynamic] */
    /* uid */		0,
    /* gid */		0,
    /* size */		0,
    /* ops */		&proc_dir_inode_operations,
  };

static struct proc_dir_entry proc_bus_i2c_dir =
  {
    /* low_ino */	0,     /* Set by proc_register_dynamic */
    /* namelen */	3, 
    /* name */		"i2c",
    /* mode */		S_IRUGO | S_IFREG,
    /* nlink */		1,     
    /* uid */		0,
    /* gid */		0,
    /* size */		0,
    /* ops */		NULL,
    /* get_info */	&read_bus_i2c
  };

#endif /* (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0)) */

/* Used by init/cleanup */
static int i2cproc_initialized;

/* This is a sorted list of all adapters that will have entries in /proc/bus */
static struct i2c_adapter *i2cproc_adapters[I2C_ADAP_MAX];

/* We will use a nasty trick: we register a driver, that will be notified
   for each adapter. Then, we register a dummy client on the adapter, that
   will get notified if the adapter is removed. This is the same trick as
   used in i2c/i2c-dev.c */
static struct i2c_driver i2cproc_driver = {
  /* name */		"i2c-proc dummy driver",
  /* id */ 		I2C_DRIVERID_I2CPROC,
  /* flags */		DF_NOTIFY,
  /* attach_adapter */	&i2cproc_attach_adapter,
  /* detach_client */   &i2cproc_detach_client,
  /* command */		&i2cproc_command,
  /* inc_use */		&i2cproc_inc_use,
  /* dec_use */		&i2cproc_dec_use
};

static struct i2c_client i2cproc_client_template = {
  /* name */		"i2c-proc dummy client",
  /* id */		1,
  /* flags */		0,
  /* addr */		-1,
  /* adapter */		NULL,
  /* driver */		&i2cproc_driver,
  /* data */		NULL
};

int i2cproc_init(void)
{
  int res;

  printk("i2c-proc.o version %s (%s)\n",LM_VERSION,LM_DATE);
  i2cproc_initialized = 0;

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0))
  if (! proc_bus) {
    printk("i2c-proc.o: /proc/bus/ does not exist, module not inserted.\n");
    i2cproc_cleanup();
    return -ENOENT;
  }
  proc_bus_i2c = create_proc_entry("i2c",0,proc_bus);
  if (proc_bus_i2c)
    proc_bus_i2c->read_proc = &read_bus_i2c;
  else {
    printk("i2c-proc.o: Could not create /proc/bus/i2c, "
           "module not inserted.\n");
    i2cproc_cleanup();
    return -ENOENT;
  }
  i2cproc_initialized += 2;
#else
  /* In Linux 2.0.x, there is no /proc/bus! But I hope no other module
     introduced it, or we are fucked. */
  if ((res = proc_register_dynamic(&proc_root, &proc_bus_dir))) {
    printk("i2c-proc.o: Could not create /proc/bus/, module not inserted.\n");
    i2cproc_cleanup();
    return res;
  }
  i2cproc_initialized ++;
  if ((res = proc_register_dynamic(&proc_bus_dir, &proc_bus_i2c_dir))) {
    printk("i2c-proc.o: Could not create /proc/bus/i2c, "
           "module not inserted.\n");
    i2cproc_cleanup();
    return res;
  }
  i2cproc_initialized ++;
#endif
  if ((res = i2c_add_driver(&i2cproc_driver))) {
    printk("i2c-proc.o: Driver registration failed, module not inserted.\n");
    i2cproc_cleanup();
    return res;
  }
  i2cproc_initialized ++;
  return 0;
}

int i2cproc_cleanup(void)
{
  int res;

  if (i2cproc_initialized >= 3) {
    if ((res = i2c_del_driver(&i2cproc_driver))) {
      printk("i2c-proc.o: Driver deregistration failed, "
             "module not removed.\n");
      return res;
    }
    i2cproc_initialized--;
  }
  if (i2cproc_initialized >= 1) {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0))
    if ((res = remove_proc_entry("i2c",proc_bus))) {
      printk("i2c-proc.o: could not delete /proc/bus/i2c, module not removed.");
      return res;
    }
    i2cproc_initialized -= 2;
#else
    if (i2cproc_initialized >= 2) {
      if ((res = proc_unregister(&proc_bus_dir,proc_bus_i2c_dir.low_ino))) {
         printk("i2c-proc.o: could not delete /proc/bus/i2c, "
                "module not removed.");
         return res;
      }    
      i2cproc_initialized --;
    }
    if ((res = proc_unregister(&proc_root,proc_bus_dir.low_ino))) {
       printk("i2c-proc.o: could not delete /proc/bus/, "
              "module not removed.");
       return res;
    }    
    i2cproc_initialized --;
#endif
  }
  return 0;
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,1,0))
int read_bus_i2c(char *buf, char **start, off_t offset, int len, int *eof, 
                 void *private)
#else
int read_bus_i2c(char *buf, char **start, off_t offset, int len, int unused)
#endif
{
  int i;
  len = 0;
  for (i = 0; i < I2C_ADAP_MAX; i++)
    if (i2cproc_adapters[i])
      len += sprintf(buf, "/dev/i2c-%d\t%s\t%-32s\t%-32s\n",i,
                     i2c_is_smbus_adapter(i2cproc_adapters[i])?"smbus":
#ifdef DEBUG
                       i2c_is_isa_adapter(i2cproc_adapters[i])?"isa":
#endif
                       "i2c",
                     i2cproc_adapters[i]->name,
                     i2cproc_adapters[i]->algo->name);
  return len;
}

/* We need to add the adapter to i2cproc_adapters, if it is interesting
   enough */
int i2cproc_attach_adapter(struct i2c_adapter *adapter)
{
  struct i2c_client *client;
  int i,res;

#ifndef DEBUG
  if (i2c_is_isa_adapter(adapter))
    return 0;
#endif /* ndef DEBUG */

  for (i = 0; i < I2C_ADAP_MAX; i++)
    if(!i2cproc_adapters[i])
      break;
  if (i == I2C_ADAP_MAX) {
    printk("i2c-proc.o: Too many adapters!\n");
    return -ENOMEM;
  }

  if (! (client = kmalloc(sizeof(struct i2c_client),GFP_KERNEL))) {
    printk("i2c-proc.o: Out of memory!\n");
    return -ENOMEM;
  }
  memcpy(client,&i2cproc_client_template,sizeof(struct i2c_client));
  client->adapter = adapter;
  if ((res = i2c_attach_client(client))) {
    printk("i2c-proc.o: Attaching client failed.\n");
    return res;
  }
  i2cproc_adapters[i] = adapter;
  return 0;
}
  
int i2cproc_detach_client(struct i2c_client *client)
{
  int i,res;

  printk("OK!\n");
#ifndef DEBUG
  if (i2c_is_isa_client(client))
    return 0;
#endif /* ndef DEBUG */

  for (i = 0; i < I2C_ADAP_MAX; i++) 
    if (client->adapter == i2cproc_adapters[i]) {
      if ((res = i2c_detach_client(client))) {
        printk("i2c-bus.o: Client deregistration failed, "
               "client not detached.\n");
        return res;
      }
      i2cproc_adapters[i] = NULL;
      kfree(client);
      return 0;
    }
  return -ENOENT;
}

/* Nothing here yet */
int i2cproc_command(struct i2c_client *client, unsigned int cmd,
                    void *arg)
{
  return -1;
}

/* Nothing here yet */
void i2cproc_inc_use(struct i2c_client *client)
{
}

/* Nothing here yet */
void i2cproc_dec_use(struct i2c_client *client)
{
}


#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>");
MODULE_DESCRIPTION("I2C /proc/bus entries driver");

int init_module(void)
{
  return i2cproc_init();
}

int cleanup_module(void)
{
  return i2cproc_cleanup();
}

#endif /* MODULE */

