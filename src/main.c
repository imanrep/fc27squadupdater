#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <microhttpd.h>

#include <ps5/kernel.h>

#include "web_ui.h"

#define TARGET_TITLE_ID "PPSA34015"
#define NOTIFICATION_MESSAGE "PPSA34015 has been opened"
#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447
#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1
#define SQUAD_SAVE_PREFIX "sdimg_Squads20"
#define EXPORT_ROOT "/data/fc27squadupdater"
#define EXPORT_MOUNT_POINT EXPORT_ROOT "/mount"
#define EXPORT_INPUT_DIR EXPORT_ROOT "/input"
#define UNPATCHED_DIR EXPORT_ROOT "/unpatched"
#define STATE_DIR EXPORT_ROOT "/state"
#define INSTANCE_LOCK EXPORT_ROOT "/updater.pid"
#define LOG_PATH EXPORT_ROOT "/updater.log"
#define LOCAL_UI_PORT 16172
#define MAX_FILE_SIZE (128U * 1024U * 1024U)

#ifndef PAYLOAD_VERSION
#define PAYLOAD_VERSION "dev"
#endif

typedef struct {
  uint32_t app_id;
  uint64_t unknown1;
  char title_id[14];
  char unknown2[0x3c];
} app_info_t;

typedef struct {
  char unused[45];
  char message[3075];
} notification_request_t;

typedef struct {
  uint8_t reserved;
  char *budgetid;
} mount_save_data_opt_t;

typedef struct {
  uint8_t reserved;
} unmount_save_data_opt_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceKernelSendNotificationRequest(int device, notification_request_t *request,
size_t request_size, int unknown);
int32_t sceSystemServiceParamGetInt(int32_t parameter_id, int32_t *value);
int sceUserServiceInitialize(void *reserved);
int sceUserServiceGetForegroundUser(int32_t *user_id);
void sceUserServiceTerminate(void);
int sceFsInitMountSaveDataOpt(mount_save_data_opt_t *opt);
int sceFsMountSaveData(mount_save_data_opt_t *opt, const char *path,
                       const char *mount_point, uint8_t *key);
int sceFsInitUmountSaveDataOpt(unmount_save_data_opt_t *opt);
int sceFsUmountSaveData(unmount_save_data_opt_t *opt, const char *mount_point,
                        int handle, int ignore);
int sceNetCtlInit(void);
int sceSystemServiceLaunchWebBrowser(const char *);

static volatile sig_atomic_t stop_requested;
static pid_t active_target_pid;
static bool owns_instance_lock;
static struct MHD_Daemon *local_ui_server;
static volatile sig_atomic_t save_ready;
static volatile sig_atomic_t save_ok_flag;
static volatile sig_atomic_t no_update_flag;
static char update_status[96] = "Waiting for update";
static char current_version[64] = "";
static char pending_version[64] = "";

static void show_popup(const char *message);

static int system_language = 1;

static bool language_is_indonesian(void) {
  return system_language == 29;
}

static const char *tr(const char *en, const char *id) {
  return language_is_indonesian() ? id : en;
}

static void detect_system_language(void) {
  int32_t language_id = 1;
  if (sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &language_id) == 0)
    system_language = language_id;
}

static void show_save_squad_first(void) {
  sleep(3);
  if (stop_requested)
    return;
  show_popup(tr("Save Squad first to get squad update",
                "Simpan Squad terlebih dahulu untuk mendapatkan update"));
  (void)sceSystemServiceLaunchWebBrowser("http://127.0.0.1:16172/?missing-save=1");
}

static void log_message(const char *format, ...) {
  char message[768];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (length <= 0) return;
  size_t size = (size_t)length < sizeof(message) - 1 ? (size_t)length : sizeof(message) - 1;
  if (size < sizeof(message) - 1) message[size++] = '\n';
  printf("%.*s", (int)size, message);
  if (mkdir(EXPORT_ROOT, 0755) != 0 && errno != EEXIST) return;
  int fd = open(LOG_PATH, O_CREAT | O_APPEND | O_WRONLY, 0644);
  if (fd < 0) return;
  size_t offset = 0;
  while (offset < size) {
    ssize_t written = write(fd, message + offset, size - offset);
    if (written <= 0) break;
    offset += (size_t)written;
  }
  close(fd);
}

static void handle_signal(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static bool process_is_alive(pid_t pid) {
  return pid > 1 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void release_instance_lock(void) {
  if (!owns_instance_lock) return;
  int fd = open(INSTANCE_LOCK, O_RDONLY);
  char lock[64] = {0};
  ssize_t count = fd >= 0 ? read(fd, lock, sizeof(lock) - 1) : -1;
  if (fd >= 0) close(fd);
  long pid = 0;
  if (count > 0 && sscanf(lock, "fc27-squad-updater:%ld", &pid) == 1 &&
      pid == (long)getpid())
    unlink(INSTANCE_LOCK);
}

static bool take_instance_lock(void) {
  if (mkdir(EXPORT_ROOT, 0755) != 0 && errno != EEXIST) return false;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    int fd = open(INSTANCE_LOCK, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0) {
      char lock[64];
      int length = snprintf(lock, sizeof(lock), "fc27-squad-updater:%ld\n",
                            (long)getpid());
      bool written = length > 0 && (size_t)length < sizeof(lock) &&
                     write(fd, lock, (size_t)length) == length && fsync(fd) == 0;
      close(fd);
      if (written) {
        owns_instance_lock = true;
        atexit(release_instance_lock);
        return true;
      }
      unlink(INSTANCE_LOCK);
      return false;
    }
    if (errno != EEXIST) return false;

    fd = open(INSTANCE_LOCK, O_RDONLY);
    char lock[64] = {0};
    ssize_t count = fd >= 0 ? read(fd, lock, sizeof(lock) - 1) : -1;
    if (fd >= 0) close(fd);
    long previous = 0;
    if (count <= 0 || sscanf(lock, "fc27-squad-updater:%ld", &previous) != 1 ||
        previous <= 1 || previous == (long)getpid()) {
      unlink(INSTANCE_LOCK);
      continue;
    }
    if (process_is_alive((pid_t)previous)) {
      (void)kill((pid_t)previous, SIGTERM);
      for (unsigned wait = 0; wait < 100 && process_is_alive((pid_t)previous); ++wait)
        usleep(100000);
    }
    if (process_is_alive((pid_t)previous)) {
      show_popup(tr("Previous updater is still stopping",
                    "elf sebelumnya masih berjalan"));
      return false;
    }
    unlink(INSTANCE_LOCK);
  }
  return false;
}

static pid_t find_pid_by_name(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t buffer_size = 0;
  if (sysctl(mib, 4, NULL, &buffer_size, NULL, 0) != 0 || buffer_size == 0)
    return -1;

  uint8_t *buffer = malloc(buffer_size);
  if (!buffer)
    return -1;

  if (sysctl(mib, 4, buffer, &buffer_size, NULL, 0) != 0) {
    free(buffer);
    return -1;
  }

  pid_t result = -1;
  for (uint8_t *entry = buffer, *end = buffer + buffer_size;
       (size_t)(end - entry) >= sizeof(int);) {
    int entry_size = 0;
    memcpy(&entry_size, entry, sizeof(entry_size));
    if (entry_size <= KINFO_TDNAME_OFFSET ||
        (size_t)entry_size > (size_t)(end - entry))
      break;

    pid_t pid = 0;
    memcpy(&pid, entry + KINFO_PID_OFFSET, sizeof(pid));
    const char *process_name = (const char *)(entry + KINFO_TDNAME_OFFSET);
    size_t process_name_size = (size_t)entry_size - KINFO_TDNAME_OFFSET;
    if (strnlen(process_name, process_name_size) < process_name_size &&
        strcmp(process_name, name) == 0) {
      result = pid;
      break;
    }
    entry += entry_size;
  }

  free(buffer);
  return result;
}

static bool process_is_target_game(pid_t pid) {
  app_info_t app_info;
  memset(&app_info, 0, sizeof(app_info));
  return sceKernelGetAppInfo(pid, &app_info) == 0 &&
         strcmp(app_info.title_id, TARGET_TITLE_ID) == 0;
}

static pid_t find_target_game_pid(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t buffer_size = 0;
  if (sysctl(mib, 4, NULL, &buffer_size, NULL, 0) != 0 || buffer_size == 0)
    return -1;

  uint8_t *buffer = malloc(buffer_size);
  if (!buffer)
    return -1;

  if (sysctl(mib, 4, buffer, &buffer_size, NULL, 0) != 0) {
    free(buffer);
    return -1;
  }

  pid_t result = -1;
  for (uint8_t *entry = buffer, *end = buffer + buffer_size;
       (size_t)(end - entry) >= sizeof(int);) {
    int entry_size = 0;
    memcpy(&entry_size, entry, sizeof(entry_size));
    if (entry_size <= KINFO_TDNAME_OFFSET ||
        (size_t)entry_size > (size_t)(end - entry))
      break;

    pid_t pid = 0;
    memcpy(&pid, entry + KINFO_PID_OFFSET, sizeof(pid));
    if (process_is_target_game(pid)) {
      result = pid;
      break;
    }
    entry += entry_size;
  }

  free(buffer);
  return result;
}

static const char *message_for_system_language(void) {
  detect_system_language();
  return tr("EA FC 27 opened", "EA FC 27 telah berjalan");
}

static void show_popup(const char *message) {
  notification_request_t request;
  memset(&request, 0, sizeof(request));
  snprintf(request.message, sizeof(request.message), "%s", message);
  (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

static bool copy_file(const char *source, const char *destination) {
  int source_fd = open(source, O_RDONLY);
  if (source_fd < 0)
    return false;

  int destination_fd = open(destination, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (destination_fd < 0) {
    close(source_fd);
    return false;
  }

  bool copied = true;
  char buffer[64 * 1024];
  ssize_t read_count;
  while ((read_count = read(source_fd, buffer, sizeof(buffer))) > 0) {
    size_t written = 0;
    while (written < (size_t)read_count) {
      ssize_t write_count =
          write(destination_fd, buffer + written, (size_t)read_count - written);
      if (write_count <= 0) {
        copied = false;
        break;
      }
      written += (size_t)write_count;
    }
    if (!copied)
      break;
  }
  if (read_count < 0)
    copied = false;

  close(destination_fd);
  close(source_fd);
  return copied;
}

static bool ensure_export_storage(void) {
  if (mkdir(EXPORT_ROOT, 0755) != 0 && errno != EEXIST)
    return false;
  if (mkdir(EXPORT_INPUT_DIR, 0755) != 0 && errno != EEXIST)
    return false;
  if (mkdir(UNPATCHED_DIR, 0755) != 0 && errno != EEXIST)
    return false;
  if (mkdir(EXPORT_MOUNT_POINT, 0755) != 0 && errno != EEXIST)
    return false;
  if (mkdir(STATE_DIR, 0755) != 0 && errno != EEXIST)
    return false;
  return true;
}

static bool find_squad_save(const char *save_directory, char save_path[1024]) {
  DIR *directory = opendir(save_directory);
  if (!directory)
    return false;

  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strncmp(entry->d_name, SQUAD_SAVE_PREFIX, strlen(SQUAD_SAVE_PREFIX)) != 0)
      continue;

    int written = snprintf(save_path, 1024, "%s/%s", save_directory, entry->d_name);
    if (written < 0 || written >= 1024)
      continue;
    struct stat status;
    if (stat(save_path, &status) == 0 && S_ISREG(status.st_mode)) {
      found = true;
      break;
    }
  }

  closedir(directory);
  return found;
}

static bool unseal_save_key(const char *save_path, uint8_t key[32]) {
  uint8_t sealed_key[0x100];
  memset(sealed_key, 0, sizeof(sealed_key));

  int input_fd = open(save_path, O_RDONLY);
  if (input_fd < 0)
    return false;
  ssize_t read_count = pread(input_fd, sealed_key, 0x60, 0x800);
  close(input_fd);
  if (read_count != 0x60)
    return false;

  int pfsmgr_fd = open("/dev/pfsmgr", O_RDWR);
  if (pfsmgr_fd < 0)
    return false;
  int result = ioctl(pfsmgr_fd, 0xc0845302, sealed_key);
  close(pfsmgr_fd);
  if (result < 0)
    return false;

  memcpy(key, sealed_key + 0x60, 32);
  return true;
}

static bool load_file(const char *path, uint8_t **out, size_t *out_size) {
  struct stat status;
  int fd;
  if (stat(path, &status) != 0 || status.st_size < 0 ||
      (uintmax_t)status.st_size > MAX_FILE_SIZE ||
      (fd = open(path, O_RDONLY)) < 0)
    return false;
  *out_size = (size_t)status.st_size;
  *out = malloc(*out_size);
  if (!*out) {
    close(fd);
    return false;
  }
  size_t offset = 0;
  while (offset < *out_size) {
    ssize_t count = read(fd, *out + offset, *out_size - offset);
    if (count <= 0) {
      close(fd);
      free(*out);
      *out = NULL;
      return false;
    }
    offset += (size_t)count;
  }
  close(fd);
  return true;
}

static bool is_version_marker_name(const char *name) {
  return strncmp(name, "Squads", 6) == 0 &&
         strchr(name, '.') == NULL &&
         strchr(name, '/') == NULL;
}

static void prune_old_version_markers(const char *keep_version) {
  if (!keep_version[0])
    return;
  DIR *directory = opendir(STATE_DIR);
  if (!directory)
    return;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (is_version_marker_name(entry->d_name) &&
        strcmp(entry->d_name, keep_version) != 0) {
      char old_marker[1024];
      int written = snprintf(old_marker, sizeof(old_marker), "%s/%s",
                             STATE_DIR, entry->d_name);
      if (written > 0 && (size_t)written < sizeof(old_marker))
        (void)unlink(old_marker);
    }
  }
  closedir(directory);
}

static void read_current_version(char *version, size_t version_size) {
  version[0] = '\0';
  DIR *directory = opendir(STATE_DIR);
  if (!directory)
    return;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (is_version_marker_name(entry->d_name) &&
        (version[0] == '\0' || strcmp(entry->d_name, version) > 0)) {
      snprintf(version, version_size, "%s", entry->d_name);
    }
  }
  closedir(directory);
  prune_old_version_markers(version);
}

static void write_current_version(const char *version) {
  if (!is_version_marker_name(version))
    return;

  char marker[1024];
  int written = snprintf(marker, sizeof(marker), "%s/%s", STATE_DIR, version);
  if (written <= 0 || (size_t)written >= sizeof(marker))
    return;
  int marker_fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (marker_fd < 0)
    return;
  close(marker_fd);
  prune_old_version_markers(version);
}

static bool read_mount_data(uint8_t **out, size_t *out_size) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/DATA", EXPORT_MOUNT_POINT);
  return load_file(path, out, out_size);
}

static bool wait_for_decrypted_data(void) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/DATA", EXPORT_MOUNT_POINT);

  off_t previous_size = -1;
  unsigned unchanged_checks = 0;
  for (unsigned attempt = 0; attempt < 60 && !stop_requested; ++attempt) {
    struct stat status;
    if (stat(path, &status) == 0 && status.st_size > 0) {
      if (status.st_size == previous_size)
        ++unchanged_checks;
      else {
        previous_size = status.st_size;
        unchanged_checks = 1;
      }
      log_message("Decrypt check %u/5: DATA size=%lld", unchanged_checks,
                  (long long)status.st_size);
      if (unchanged_checks >= 5)
        return true;
    } else {
      previous_size = -1;
      unchanged_checks = 0;
    }
    sleep(1);
  }
  return false;
}

static bool mount_squad_save(const char *source_path, char local_save_path[1024]) {
  const char *source_name = strrchr(source_path, '/');
  source_name = source_name ? source_name + 1 : source_path;

  int local_written = snprintf(local_save_path, 1024, "%s/%s", EXPORT_INPUT_DIR,
                               source_name);
  if (local_written < 0 || (size_t)local_written >= 1024 ||
      !copy_file(source_path, local_save_path))
    return false;

  kernel_set_ucred_authid(getpid(), 0x4800000000000010ULL);
  uint8_t capabilities[16];
  if (kernel_get_ucred_caps(getpid(), capabilities) == 0) {
    capabilities[7] |= 0x40;
    (void)kernel_set_ucred_caps(getpid(), capabilities);
  }
  (void)setuid(0);

  uint8_t key[32];
  if (!unseal_save_key(local_save_path, key)) {
    unlink(local_save_path);
    return false;
  }

  mount_save_data_opt_t mount_options;
  memset(&mount_options, 0, sizeof(mount_options));
  (void)sceFsInitMountSaveDataOpt(&mount_options);
  mount_options.budgetid = "system";
  int mount_result =
      sceFsMountSaveData(&mount_options, local_save_path, EXPORT_MOUNT_POINT, key);
  if (mount_result < 0) {
    unlink(local_save_path);
    return false;
  }
  return true;
}

static int unmount_squad_save(void) {
  unmount_save_data_opt_t unmount_options;
  memset(&unmount_options, 0, sizeof(unmount_options));
  (void)sceFsInitUmountSaveDataOpt(&unmount_options);
  int result = sceFsUmountSaveData(&unmount_options, EXPORT_MOUNT_POINT, 0, 0);
  log_message("Unmount result=%d", result);
  return result;
}

struct upload_ctx {
  int fd;
  bool failed;
  size_t written;
};

static enum MHD_Result local_ui_request(void *cls, struct MHD_Connection *connection,
    const char *url, const char *method, const char *version, const char *upload,
    size_t *upload_size, void **connection_cls) {
  (void)cls;
  (void)version;

  if (*connection_cls == NULL)
    log_message("HTTP: %s %s", method, url);

  if (strcmp(method, "POST") == 0 && strcmp(url, "/api/data") == 0) {
    struct upload_ctx *ctx = *connection_cls;
    if (ctx == NULL) {
      ctx = calloc(1, sizeof(*ctx));
      if (!ctx)
        return MHD_NO;
      char data_path[1024];
      snprintf(data_path, sizeof(data_path), "%s/DATA", EXPORT_MOUNT_POINT);
      ctx->fd = open(data_path, O_TRUNC | O_WRONLY);
      ctx->failed = ctx->fd < 0;
      if (ctx->failed)
        log_message("DATA open failed errno=%d", errno);
      *connection_cls = ctx;
      // MHD's first callback initializes per-request state before body chunks arrive.
      return MHD_YES;
    }

    if (*upload_size != 0) {
      if (ctx->fd >= 0) {
        size_t offset = 0;
        while (offset < *upload_size) {
          ssize_t count = write(ctx->fd, upload + offset, *upload_size - offset);
          if (count <= 0) {
            ctx->failed = true;
            break;
          }
          offset += (size_t)count;
        }
        ctx->written += offset;
      } else {
        ctx->failed = true;
      }
      *upload_size = 0;
      return MHD_YES;
    }

    if (ctx->fd >= 0) {
      fsync(ctx->fd);
      close(ctx->fd);
    }
    log_message("POST finalize: failed=%d written=%zu", ctx->failed, ctx->written);
    if (!ctx->failed)
      save_ok_flag = 1;
    free(ctx);
    const char *new_version =
        MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-FC27-Version");
    if (new_version && new_version[0])
      snprintf(pending_version, sizeof(pending_version), "%s", new_version);
    save_ready = 1;

    const char *ok = "OK";
    struct MHD_Response *response =
        MHD_create_response_from_buffer(2, (void *)ok, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
  }

  if (strcmp(method, "POST") == 0 && strcmp(url, "/api/no-update") == 0) {
    no_update_flag = 1;
    save_ready = 1;
    const char *ok = "OK";
    struct MHD_Response *response =
        MHD_create_response_from_buffer(2, (void *)ok, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
  }

  if (strcmp(method, "GET") != 0) {
    struct MHD_Response *response =
        MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result result =
        MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return result;
  }

  const char *body = NULL;
  size_t body_size = 0;
  const char *content_type = "text/plain; charset=utf-8";
  bool free_body = false;

  if (strcmp(url, "/") == 0) {
    body = local_page;
    body_size = sizeof(local_page) - 1;
    content_type = "text/html; charset=utf-8";
  } else if (strcmp(url, "/logo.svg") == 0) {
    body = local_logo;
    body_size = sizeof(local_logo) - 1;
    content_type = "image/svg+xml";
  } else if (strcmp(url, "/circle.png") == 0) {
    body = (const char *)local_circle;
    body_size = local_circle_size;
    content_type = "image/png";
  } else if (strcmp(url, "/square.png") == 0) {
    body = (const char *)local_square;
    body_size = local_square_size;
    content_type = "image/png";
  } else if (strcmp(url, "/save.jpg") == 0) {
    body = (const char *)local_save;
    body_size = local_save_size;
    content_type = "image/jpeg";
  } else if (strcmp(url, "/setting.png") == 0) {
    body = (const char *)local_setting;
    body_size = local_setting_size;
    content_type = "image/png";
  } else if (strcmp(url, "/api/lang") == 0) {
    body = language_is_indonesian() ? "id" : "en";
    body_size = strlen(body);
  } else if (strcmp(url, "/api/version") == 0) {
    body = current_version;
    body_size = strlen(current_version);
  } else if (strcmp(url, "/api/status") == 0) {
    body = update_status;
    body_size = strlen(update_status);
  } else if (strcmp(url, "/api/data") == 0) {
    uint8_t *data = NULL;
    size_t size = 0;
    if (read_mount_data(&data, &size)) {
      body = (const char *)data;
      body_size = size;
      content_type = "application/octet-stream";
      free_body = true;
    } else {
      body = "DATA not available";
      body_size = strlen(body);
    }
  } else {
    body = "not found";
    body_size = strlen(body);
  }

  struct MHD_Response *response = MHD_create_response_from_buffer(
      body_size, (void *)body, free_body ? MHD_RESPMEM_MUST_FREE : MHD_RESPMEM_PERSISTENT);
  MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
  MHD_add_response_header(response, "Content-Type", content_type);
  enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return result;
}

static bool start_local_ui(void) {
  if (local_ui_server)
    return true;
  int network_result = sceNetCtlInit();
  if (network_result != 0)
    log_message("sceNetCtlInit returned %d", network_result);

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_port = htons(LOCAL_UI_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  local_ui_server = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, 0, NULL, NULL,
                                     &local_ui_request, NULL,
                                     MHD_OPTION_SOCK_ADDR, (intptr_t)&address,
                                     MHD_OPTION_LISTENING_ADDRESS_REUSE, 1,
                                     MHD_OPTION_END);
  if (!local_ui_server)
    log_message("Couldn't bind local UI port %d (errno=%d)", LOCAL_UI_PORT, errno);
  return local_ui_server != NULL;
}

static void run_squad_update(pid_t game_pid) {
  int32_t user_id = 0;
  if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id < 0) {
    show_popup(tr("Couldn't find save game folder",
                  "Folder penyimpanan game tidak ditemukan"));
    return;
  }

  char save_path[256];
  int written = snprintf(save_path, sizeof(save_path),
                         "/user/home/%08x/savedata_prospero/%s",
                         (uint32_t)user_id, TARGET_TITLE_ID);
  if (written < 0 || (size_t)written >= sizeof(save_path)) {
    show_popup(tr("Couldn't find save game folder",
                  "Folder penyimpanan game tidak ditemukan"));
    return;
  }

  struct stat status;
  if (stat(save_path, &status) != 0 || !S_ISDIR(status.st_mode)) {
    show_save_squad_first();
    return;
  }

  char squad_save_path[1024];
  if (!find_squad_save(save_path, squad_save_path)) {
    show_save_squad_first();
    return;
  }

  read_current_version(current_version, sizeof(current_version));
  snprintf(update_status, sizeof(update_status), "%s",
           tr("Waiting for update", "Menunggu pembaruan"));
  pending_version[0] = '\0';
  save_ready = 0;
  save_ok_flag = 0;
  no_update_flag = 0;

  log_message("Mounting squad save");
  char local_save_path[1024];
  if (!mount_squad_save(squad_save_path, local_save_path)) {
    show_popup(tr("Couldn't mount squad save",
                  "Tidak dapat memasang penyimpanan squad"));
    return;
  }
  log_message("Squad save mounted (version=%s)",
              current_version[0] ? current_version : "(none)");

  if (!wait_for_decrypted_data()) {
    show_popup(tr("Decrypted DATA was not ready",
                  "DATA terdekripsi belum siap"));
    return;
  }

  char unpatched_data_path[1024];
  snprintf(unpatched_data_path, sizeof(unpatched_data_path), "%s/DATA", UNPATCHED_DIR);
  char mounted_data_path[1024];
  snprintf(mounted_data_path, sizeof(mounted_data_path), "%s/DATA", EXPORT_MOUNT_POINT);
  if (!copy_file(mounted_data_path, unpatched_data_path)) {
    show_popup(tr("Couldn't back up unpatched DATA",
                  "Tidak dapat mencadangkan DATA asli"));
    return;
  }
  log_message("Saved unpatched DATA to %s", unpatched_data_path);

  log_message("Opening updater page");
  if (sceSystemServiceLaunchWebBrowser("http://127.0.0.1:16172/") != 0)
    show_popup(tr("Couldn't open squad updater",
                  "Tidak dapat membuka squad updater"));

  for (unsigned wait = 0; wait < 2400 && !save_ready && !stop_requested; ++wait) {
    usleep(50000);
    if ((wait % 20) == 0 && !process_is_alive(game_pid)) {
      log_message("Game exited while updating");
      break;
    }
  }

  bool no_update = save_ready && no_update_flag;
  bool patched = save_ready && save_ok_flag;
  log_message("Update session finished: patched=%d no_update=%d", patched, no_update);

  if (patched)
    sync();

  unmount_squad_save();

  if (no_update) {
    snprintf(update_status, sizeof(update_status), "%s",
             tr("Already up to date", "Squad sudah yang terbaru"));
    return;
  }

  if (!patched) {
    (void)unlink(local_save_path);
    if (stop_requested) {
      log_message("Update aborted by shutdown");
      return;
    }
    snprintf(update_status, sizeof(update_status), "%s",
             tr("Update failed", "Pembaruan gagal"));
    show_popup(tr("Squad update failed", "Pembaruan squad gagal"));
    return;
  }

  struct stat local_status;
  int stat_result = stat(local_save_path, &local_status);
  log_message("Local save after unmount: stat=%d size=%lld",
              stat_result, stat_result == 0 ? (long long)local_status.st_size : -1);

  bool saved = copy_file(local_save_path, squad_save_path);
  log_message("Copy back result=%d", saved);

  if (!saved) {
    snprintf(update_status, sizeof(update_status), "%s",
             tr("Update failed", "Pembaruan gagal"));
    show_popup(tr("Couldn't update squad save",
                  "Tidak dapat memperbarui penyimpanan squad"));
    return;
  }

  if (pending_version[0]) {
    write_current_version(pending_version);
  }

  show_popup(tr("Squad update complete. Restart FC27 to apply the patch.",
                "Pembaruan squad selesai. Buka kembali FC27 untuk menerapkan."));
  snprintf(update_status, sizeof(update_status), "%s",
           tr("Squad update complete", "Pembaruan squad selesai"));
  if (kill(game_pid, SIGTERM) != 0)
    log_message("Couldn't close game pid %ld errno=%d", (long)game_pid, errno);
  else
    log_message("Closed game pid %ld after squad update", (long)game_pid);
}

static bool watch_process_exit(int kqueue_fd, pid_t pid) {
  struct kevent event;
  EV_SET(&event, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_EXIT, 0, NULL);
  return kevent(kqueue_fd, &event, 1, NULL, 0, NULL) == 0;
}

int main(void) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  if (!take_instance_lock()) {
    printf("Another FC27 squad updater is still active\n");
    return 1;
  }
  detect_system_language();
  show_popup(tr("EA FC27 Squad Updater loaded.", "EA FC27 Squad Updater dimuat."));
  (void)ensure_export_storage();
  (void)sceUserServiceInitialize(NULL);
  if (!start_local_ui()) {
    show_popup(tr("Couldn't start squad updater",
                  "Tidak dapat memulai pembaru squad"));
    return 1;
  }
  log_message("Local UI listening on 127.0.0.1:%d", LOCAL_UI_PORT);

  pid_t syscore_pid = find_pid_by_name("SceSysCore.elf");
  if (syscore_pid <= 0) {
    printf("Could not find SceSysCore.elf\n");
    return 1;
  }

  int kqueue_fd = kqueue();
  if (kqueue_fd < 0) {
    perror("kqueue");
    return 1;
  }

  struct kevent event;
  EV_SET(&event, (uintptr_t)syscore_pid, EVFILT_PROC, EV_ADD | EV_ENABLE |
             EV_CLEAR, NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, NULL);
  if (kevent(kqueue_fd, &event, 1, NULL, 0, NULL) != 0) {
    perror("kevent registration");
    close(kqueue_fd);
    return 1;
  }

  printf("EA FC 27 squad updater v%s watching for %s\n", PAYLOAD_VERSION,
         TARGET_TITLE_ID);

  pid_t existing_game = find_target_game_pid();
  if (existing_game > 0) {
    active_target_pid = existing_game;
    (void)watch_process_exit(kqueue_fd, existing_game);
    show_popup(message_for_system_language());
    printf("Detected running %s (pid %ld)\n", TARGET_TITLE_ID, (long)existing_game);
    run_squad_update(existing_game);
    active_target_pid = 0;
  }

  while (!stop_requested) {
    struct kevent received_event;
    int count = kevent(kqueue_fd, NULL, 0, &received_event, 1, NULL);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      perror("kevent wait");
      break;
    }
    if (count == 0 || received_event.filter != EVFILT_PROC)
      continue;

    pid_t pid = (pid_t)received_event.ident;
    if ((received_event.fflags & NOTE_EXIT) != 0 && pid == active_target_pid) {
      active_target_pid = 0;
      continue;
    }
    if ((received_event.fflags & NOTE_EXEC) == 0 ||
        !process_is_target_game(pid))
      continue;

    if (active_target_pid == pid)
      continue;

    if (active_target_pid != 0)
      active_target_pid = 0;

    active_target_pid = pid;
    (void)watch_process_exit(kqueue_fd, pid);
    show_popup(message_for_system_language());
    printf("Detected %s (pid %ld)\n", TARGET_TITLE_ID, (long)pid);
    run_squad_update(pid);
    active_target_pid = 0;
  }

  close(kqueue_fd);
  if (local_ui_server)
    MHD_stop_daemon(local_ui_server);
  sceUserServiceTerminate();
  return 0;
}
