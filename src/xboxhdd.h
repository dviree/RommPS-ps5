/* The Xbox hard disk xemu keeps every game's saves on: a qcow2 image (or a
 * raw one), its E: partition FATX. A save is the folder E:\UDATA\<title id>,
 * "4d530004", and moves between Xboxes by copying that folder, which is the
 * form RomM's save units give it (argosy-sigil, docs/platforms/xbox.md).
 * E:\TDATA is the game's cache and stays where it is. */
#ifndef ROMM_XBOXHDD_H
#define ROMM_XBOXHDD_H

typedef struct xhdd xhdd;

/* NULL when it isn't an image this can read (a backing file, encryption,
 * compressed clusters, no FATX at E:). */
xhdd *xhdd_open(const char *image, int writable);
void xhdd_close(xhdd *h);

/* Copies E:\UDATA\<id> (any case) into dir as dir/<its name>/..., each file
 * keeping its time on the disk. 1 when copied, 0 when the game has no save,
 * -1 when the disk can't be read. */
int xhdd_get_save(xhdd *h, const char *id, const char *dir);

/* E:\UDATA\<id> on the image becomes the folder from (the save folder
 * itself, not its parent). The image is changed as a copy that replaces it
 * when all went in; on failure it's as it was. Never while xemu runs. */
int xhdd_put_save_file(const char *image, const char *id, const char *from);

#endif
