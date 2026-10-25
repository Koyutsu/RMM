/* module_main.c - fs module entry.  Subsystem init happens
 * through explicit calls (vfs_init, ramfs_heap_init) from the core boot
 * sequence so the boot log shows demand-load ordering clearly. */
int module_entry(void) { return 0; }
