/* Crash-safe save writes: a save file opened for writing is a temporary copy that replaces the
 * file when closed (src/runtime_file.c).
 * Windows (MSYS2 CLANG64): clang -std=c11 -pthread -Isrc tests/test_save_commit.c src/compat_win.c -lbcrypt -lws2_32 */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void put(const char *path,const char *text) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static int has(const char *path,const char *text) {
    char buffer[256]={0};
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    size_t n=fread(buffer,1,sizeof(buffer)-1,f);
    fclose(f);
    return n==strlen(text) && !memcmp(buffer,text,n);
}
static int exists(const char *path) { struct stat s; return !stat(path,&s); }
#define O_GRDWR 2
#define O_GWRONLY 1
#define O_GCREAT 0x200
#define O_GTRUNC 0x400
int main(void) {
    char root[512],mount[600],file[700],other[700],stale[700],tmp[700];
#ifdef _WIN32
    const char *base=getenv("TEMP"); if (!base) base=".";
    snprintf(root,sizeof(root),"%s/bbport-save-commit-%d",base,(int)getpid());
    assert(!mkdir(root,0755));
#else
    snprintf(root,sizeof(root),"/tmp/bbport-save-commit-%d",(int)getpid());
    assert(!mkdir(root,0755));
#endif
    snprintf(mount,sizeof(mount),"%s/save",root);
    assert(!mkdir(mount,0755));
    snprintf(file,sizeof(file),"%s/slot.bin",mount);
    snprintf(other,sizeof(other),"%s/other.bin",mount);
    snprintf(stale,sizeof(stale),"%s/old.bin.3.bbtmp",mount);
    put(stale,"junk");
    put(file,"old-content-0123456789");
    assert(exists(stale));
    assert(!runtime_file_mount("/savedata0",mount));
    assert(!exists(stale)); /* an interrupted save's copy is removed when its directory is mounted */

    /* overwrite in place (O_RDWR, write at an offset): the file keeps the old content until close */
    int fd=(int)do_open("/savedata0/slot.bin",O_GRDWR,0); assert(fd>=3);
    assert(do_pwrite(fd,"NEW",3,4)==3);
    assert(has(file,"old-content-0123456789"));
    char got[32]={0};
    assert(do_pread(fd,got,10,0)==10 && !memcmp(got,"old-NEWten",10));
    GuestStat st; assert(!do_stat("/savedata0/slot.bin",&st) && st.size==22);
    assert(!do_close(fd));
    assert(has(file,"old-NEWtent-0123456789"));

    /* a crash before close leaves the old file and a copy that the next mount removes */
    fd=(int)do_open("/savedata0/slot.bin",O_GWRONLY|O_GTRUNC,0); assert(fd>=3);
    assert(do_write(fd,"half",4)==4);
    assert(has(file,"old-NEWtent-0123456789"));
    DIR *d=opendir(mount); int copies=0;
    for (struct dirent *e; (e=readdir(d));) if (temp_name(e->d_name)) { ++copies; snprintf(tmp,sizeof(tmp),"%s/%s",mount,e->d_name); }
    closedir(d);
    assert(copies==1);
    int dirfd=(int)do_open("/savedata0",0x20000,0); assert(dirfd>=3);
    char entries[1024]; int64_t count=do_getdents(dirfd,entries,sizeof(entries),NULL); assert(count>0);
    for (int64_t p=0;p<count;) { uint16_t length; memcpy(&length,entries+p+4,2); assert(length); assert(!temp_name(entries+p+8)); p+=length; }
    assert(!do_close(dirfd));
    assert(!do_close(fd));
    assert(has(file,"half")); /* truncated and written: committed on close */
    assert(!exists(tmp));

    /* opened to truncate and closed unwritten: emptied, as without the copy */
    fd=(int)do_open("/savedata0/slot.bin",O_GWRONLY|O_GTRUNC,0); assert(fd>=3);
    assert(!do_close(fd));
    assert(has(file,""));

    /* opened read-write and only read: untouched, no copy left behind */
    put(file,"keep");
    fd=(int)do_open("/savedata0/slot.bin",O_GRDWR,0); assert(fd>=3);
    assert(do_read(fd,got,4)==4);
    assert(!do_close(fd));
    assert(has(file,"keep"));

    /* created and closed unwritten: an empty file exists */
    fd=(int)do_open("/savedata0/other.bin",O_GWRONLY|O_GCREAT,0644); assert(fd>=3);
    assert(!do_close(fd));
    assert(exists(other) && has(other,""));

    /* created and written; ftruncate counts as a write */
    unlink(other);
    fd=(int)do_open("/savedata0/other.bin",O_GRDWR|O_GCREAT,0644); assert(fd>=3);
    assert(do_write(fd,"abcdef",6)==6 && !do_ftruncate(fd,3));
    assert(!do_close(fd));
    assert(has(other,"abc"));

    /* a reader that opens the file while it is being written reads the copy (the game stats and
       reopens the file it writes), and the file is replaced when the writer closes */
    put(file,"before");
    int w=(int)do_open("/savedata0/slot.bin",O_GWRONLY|O_GTRUNC,0); assert(w>=3);
    assert(do_write(w,"after",5)==5);
    int r=(int)do_open("/savedata0/slot.bin",0,0); assert(r>=3);
    assert(do_read(r,got,5)==5 && !memcmp(got,"after",5));
    assert(!do_close(r));
    assert(has(file,"before"));
    assert(!do_close(w));
    assert(has(file,"after"));

    unlink(file); unlink(other);
    assert(!rmdir(mount) && !rmdir(root));
    puts("Save commit: copy, replace on close, crash, truncate, create PASS");
    return 0;
}
