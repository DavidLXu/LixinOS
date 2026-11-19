#include "../console.h"
#include "../file-time.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/stream.h"
#include "py/objtuple.h"
#include "py/builtin.h"
#include "py/mphal.h"
#include <string.h>

#define MAIL ((char *)0x443d7000)
#define CHUNK 4096
#define DIR_MODE 0040000
#define FILE_MODE 0100000

static const char *path_arg(mp_obj_t arg) {
    size_t length;
    const char *path = mp_obj_str_get_data(arg, &length);
    if (length > 512) mp_raise_OSError(MP_EINVAL);
    if (!length || memchr(path, 0, length)) mp_raise_OSError(MP_EINVAL);
    return path;
}

static void request(const char *text) {
    size_t length = strlen(text);
    memcpy(MAIL, text, length + 1);
}

static void check(long result) {
    if (result < 0) mp_raise_OSError(-result);
}

static mp_obj_t os_getcwd(void) {
    long n = console_call(42, 0);
    if (n < 0 || n > 512) mp_raise_OSError(MP_EPERM);
    return mp_obj_new_str(MAIL, n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t directory_operation(mp_obj_t path, int operation) {
    request(path_arg(path));
    check(console_call(42, operation));
    return mp_const_none;
}

static mp_obj_t os_chdir(mp_obj_t path) { return directory_operation(path, 1); }
static mp_obj_t os_mkdir(mp_obj_t path) { return directory_operation(path, 2); }
static mp_obj_t os_remove(mp_obj_t path) { return directory_operation(path, 3); }
static mp_obj_t os_rmdir(mp_obj_t path) { return directory_operation(path, 4); }
static MP_DEFINE_CONST_FUN_OBJ_1(os_chdir_obj, os_chdir);
static MP_DEFINE_CONST_FUN_OBJ_1(os_mkdir_obj, os_mkdir);
static MP_DEFINE_CONST_FUN_OBJ_1(os_remove_obj, os_remove);
static MP_DEFINE_CONST_FUN_OBJ_1(os_rmdir_obj, os_rmdir);

static void check_metadata(long result) {
    if (result >= 0 || result == -2) return;
    if (result == -3) mp_raise_OSError(MP_ENOENT);
    if (result == -4) mp_raise_OSError(MP_ENOTDIR);
    mp_raise_OSError(MP_EINVAL);
}

static mp_obj_t os_stat(mp_obj_t path) {
    request(path_arg(path));
    long size = console_call(7, 0);
    // The restricted project test runner rejects filesystem calls.
    if (size == -1) mp_raise_OSError(MP_EPERM);
    check_metadata(size);
    mp_obj_t fields[10];
    for (size_t i = 0; i < MP_ARRAY_SIZE(fields); ++i) fields[i] = MP_OBJ_NEW_SMALL_INT(0);
    fields[0] = MP_OBJ_NEW_SMALL_INT(size == -2 ? DIR_MODE : FILE_MODE);
    fields[6] = MP_OBJ_NEW_SMALL_INT(size < 0 ? 0 : size);
    fields[8] = mp_obj_new_int_from_uint(lx_file_time(1));
    return mp_obj_new_tuple(MP_ARRAY_SIZE(fields), fields);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

static mp_obj_t os_listdir(size_t n_args, const mp_obj_t *args) {
    const char *path = !n_args || args[0] == mp_const_none ? "." : path_arg(args[0]);
    request(path);
    long kind = console_call(7, 0);
    if (kind == -1) mp_raise_OSError(MP_EPERM);
    check_metadata(kind);
    if (kind != -2) mp_raise_OSError(MP_ENOTDIR);
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (unsigned index = 1; ; ++index) {
        request(path);
        long status = console_call(7, index);
        if (status == -1) break;
        check_metadata(status);
        mp_obj_list_append(result, mp_obj_new_str(MAIL + 513, strlen(MAIL + 513)));
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

static mp_obj_t system_field(unsigned field) {
    long n = console_call(29, field);
    if (n < 0 || n > CHUNK) mp_raise_OSError(MP_EPERM);
    return mp_obj_new_str(MAIL, n);
}

static mp_obj_t os_uname(void) {
    static const qstr names[] = {
        MP_QSTR_sysname, MP_QSTR_nodename, MP_QSTR_release, MP_QSTR_version, MP_QSTR_machine,
    };
    mp_obj_t fields[MP_ARRAY_SIZE(names)];
    fields[0] = fields[1] = system_field(1);
    fields[2] = system_field(2);
    fields[3] = system_field(4);
    fields[4] = system_field(3);
    return mp_obj_new_attrtuple(names, MP_ARRAY_SIZE(fields), fields);
}
static MP_DEFINE_CONST_FUN_OBJ_0(os_uname_obj, os_uname);

static mp_obj_t os_system(mp_obj_t arg) {
    size_t length;
    const char *command = mp_obj_str_get_data(arg, &length);
    if (length > 512 || memchr(command, 0, length)) mp_raise_OSError(MP_EINVAL);
    request(command);
    long result = console_call(44, 0);
    if (result < 0) mp_raise_OSError(MP_EPERM);
    return MP_OBJ_NEW_SMALL_INT(result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_system_obj, os_system);

static mp_obj_t os_urandom(mp_obj_t arg) {
    mp_int_t length = mp_obj_get_int(arg);
    if (length < 0 || length > CHUNK) mp_raise_ValueError(MP_ERROR_TEXT("expected 0 to 4096 random bytes"));
    if (length && console_call(30, length) < 0) mp_raise_OSError(MP_EIO);
    return mp_obj_new_bytes((const byte *)MAIL, length);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_urandom_obj, os_urandom);

unsigned lixin_random_seed(void) {
    unsigned seed = 1;
    if (console_call(30, sizeof(seed)) == 0) memcpy(&seed, MAIL, sizeof(seed));
    return seed;
}

static mp_uint_t clock_ticks(int unit) {
    long result = console_call(38, unit);
    if (result < 0) mp_raise_OSError(MP_EPERM);
    return result;
}
mp_uint_t mp_hal_ticks_ms(void) { return clock_ticks(0); }
mp_uint_t mp_hal_ticks_us(void) { return clock_ticks(1); }
mp_uint_t mp_hal_ticks_cpu(void) { return clock_ticks(2); }
uint64_t mp_hal_time_ns(void) {
    long seconds = console_call(36, 0);
    if (seconds < 0) mp_raise_OSError(MP_EPERM);
    return (uint64_t)seconds * 1000000000ULL;
}

void mp_hal_delay_ms(mp_uint_t ms) {
    if (console_call(43, ms) < 0) mp_raise_OSError(MP_EPERM);
}
void mp_hal_delay_us(mp_uint_t us) { mp_hal_delay_ms(us / 1000 + (us % 1000 != 0)); }
void lixin_wait_for_event(void) { mp_hal_delay_ms(1); }

typedef struct {
    mp_obj_base_t base;
    int fd;
    bool writing;
} GuestFile;

static mp_uint_t file_read(mp_obj_t self, void *buf, mp_uint_t size, int *error) {
    GuestFile *file = MP_OBJ_TO_PTR(self);
    if (file->fd < 0 || file->writing) { *error = MP_EBADF; return MP_STREAM_ERROR; }
    if (file->fd == 0) {
        if (size == 0) return 0;
        int byte = get_byte();
        if (byte == 256) return 0;
        if (byte < 0 || byte > 255) { *error = MP_EIO; return MP_STREAM_ERROR; }
        *(unsigned char *)buf = byte;
        return 1;
    }
    if (size > CHUNK) size = CHUNK;
    long n = console_call(9, (unsigned long)file->fd << 32 | size);
    if (n < 0 || (mp_uint_t)n > size) { *error = MP_EIO; return MP_STREAM_ERROR; }
    memcpy(buf, MAIL, n);
    return n;
}

static mp_uint_t file_write(mp_obj_t self, const void *buf, mp_uint_t size, int *error) {
    GuestFile *file = MP_OBJ_TO_PTR(self);
    if (file->fd < 0 || !file->writing) { *error = MP_EBADF; return MP_STREAM_ERROR; }
    if (file->fd == 1) return mp_hal_stdout_tx_strn(buf, size);
    if (file->fd == 2) {
        const unsigned char *bytes = buf;
        for (mp_uint_t i = 0; i < size; ++i) {
            if (console_call(11, bytes[i]) < 0) { *error = MP_EIO; return MP_STREAM_ERROR; }
        }
        return size;
    }
    if (size > CHUNK) size = CHUNK;
    memcpy(MAIL, buf, size);
    long n = console_call(39, (unsigned long)file->fd << 32 | size);
    if (n < 0 || (mp_uint_t)n > size) { *error = MP_EIO; return MP_STREAM_ERROR; }
    return n;
}

static mp_uint_t file_ioctl(mp_obj_t self, mp_uint_t op, uintptr_t arg, int *error) {
    GuestFile *file = MP_OBJ_TO_PTR(self);
    if (op == MP_STREAM_CLOSE) {
        if (file->fd >= 3) {
            console_call(10, file->fd);
            file->fd = -1;
        }
        return 0;
    }
    if (op == MP_STREAM_FLUSH && file->fd >= 0) return 0;
    if (op == MP_STREAM_POLL && file->fd >= 0) {
        unsigned event = file->writing ? MP_STREAM_POLL_WR : MP_STREAM_POLL_RD;
        if (!(arg & event)) return 0;
        unsigned mask = file->writing ? 2 : 1;
        long ready = console_call(41, (unsigned long)file->fd << 48 | (unsigned long)mask << 32);
        if (ready < 0) { *error = MP_EBADF; return MP_STREAM_ERROR; }
        return ready ? event : 0;
    }
    *error = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_rom_map_elem_t file_methods_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_methods, file_methods_table);
static const mp_stream_p_t binary_stream = {
    .read = file_read, .write = file_write, .ioctl = file_ioctl,
};
static const mp_stream_p_t text_stream = {
    .read = file_read, .write = file_write, .ioctl = file_ioctl, .is_text = true,
};
static MP_DEFINE_CONST_OBJ_TYPE(binary_file, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &binary_stream, locals_dict, &file_methods);
static MP_DEFINE_CONST_OBJ_TYPE(text_file, MP_QSTR_TextIOWrapper, MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &text_stream, locals_dict, &file_methods);

const GuestFile mp_sys_stdin_obj = { { &text_file }, 0, false };
const GuestFile mp_sys_stdout_obj = { { &text_file }, 1, true };
const GuestFile mp_sys_stderr_obj = { { &text_file }, 2, true };

static mp_obj_t open_file(size_t n_args, const mp_obj_t *args, mp_map_t *keywords) {
    static const mp_arg_t allowed[] = {
        { MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mode, MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_r)} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, args, keywords, MP_ARRAY_SIZE(allowed), allowed, parsed);
    const char *path = path_arg(parsed[0].u_obj);
    size_t len;
    const char *mode = mp_obj_str_get_data(parsed[1].u_obj, &len);
    if (!len || len > 2 || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')
        || (len == 2 && mode[1] != 't' && mode[1] != 'b'))
        mp_raise_ValueError(MP_ERROR_TEXT("expected r, w or a, optionally with b or t"));
    GuestFile *file = mp_obj_malloc_with_finaliser(GuestFile,
        len == 2 && mode[1] == 'b' ? &binary_file : &text_file);
    file->writing = mode[0] != 'r';
    file->fd = -1;
    request(path);
    long size = console_call(7, 0);
    if (size == -1) mp_raise_OSError(MP_EPERM);
    if (!file->writing || size != -3) check_metadata(size);
    if (size == -2) mp_raise_OSError(MP_EISDIR);
    file->fd = console_call(file->writing ? 40 : 8, mode[0] == 'a');
    if (file->fd < 0) mp_raise_OSError(MP_EIO);
    return MP_OBJ_FROM_PTR(file);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 0, open_file);

static const mp_rom_map_elem_t os_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink), MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&os_uname_obj) },
    { MP_ROM_QSTR(MP_QSTR_system), MP_ROM_PTR(&os_system_obj) },
    { MP_ROM_QSTR(MP_QSTR_urandom), MP_ROM_PTR(&os_urandom_obj) },
};
static MP_DEFINE_CONST_DICT(os_globals, os_globals_table);
const mp_obj_module_t lixin_os_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_globals,
};
MP_REGISTER_MODULE(MP_QSTR_os, lixin_os_module);
MP_REGISTER_MODULE(MP_QSTR_uos, lixin_os_module);
