#ifndef LX_IDENTITY_H
#define LX_IDENTITY_H
#define LX_PERMISSIONS "/.lixin-permissions"
#define LX_GROUPS 32
#define LX_PERM_LIMIT (2U<<20)
struct lx_mode { unsigned uid, gid, mode; };
/* 64/0 query ABI, 1 read six IDs, 2 setresuid, 3 setresgid, 4 setgroups,
 * 5 getgroups, 6 umask, 7 setuid, 8 setgid, 9 setreuid, 10 setregid. */
long id_call(unsigned owner, unsigned op, unsigned *words);
void id_inherit(unsigned child, unsigned parent);
unsigned id_uid(unsigned owner);
unsigned id_gid(unsigned owner);
unsigned id_mask(unsigned owner);
int id_access(unsigned owner, const struct lx_mode *file, unsigned access);
void id_default(const char *path, unsigned size, int directory, struct lx_mode *out);
/* Packed metadata records are little-endian uid,gid,mode,path size then path.
 * Negative results reject malformed data, including truncated trailing records. */
long id_find(const unsigned char *data, unsigned size, const char *path,
             unsigned length, struct lx_mode *out, unsigned *offset);
int id_mode(const unsigned char *, unsigned, const char *, unsigned, int, struct lx_mode *);
long id_encode(const unsigned char *,unsigned,const char *,unsigned,const struct lx_mode *,
               unsigned char *,unsigned);
int id_path(unsigned,const unsigned char *,unsigned,const char *,unsigned,int,unsigned);
int id_modify(unsigned,struct lx_mode *,unsigned,unsigned,unsigned);
#endif
