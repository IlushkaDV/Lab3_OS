#include "platform.h"
#include <sstream>
#include <iomanip>

Timestamp get_current_timestamp() {
    Timestamp ts = {0};
    
#ifdef _WIN32
    struct _timeb timebuffer;
    _ftime64(&timebuffer);
    struct tm tm_struct;
    localtime_s(&tm_struct, &timebuffer.time);
    
    ts.year = tm_struct.tm_year + 1900;
    ts.month = tm_struct.tm_mon + 1;
    ts.day = tm_struct.tm_mday;
    ts.hour = tm_struct.tm_hour;
    ts.minute = tm_struct.tm_min;
    ts.second = tm_struct.tm_sec;
    ts.millisecond = timebuffer.millitm;
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_struct;
    localtime_r(&tv.tv_sec, &tm_struct);
    
    ts.year = tm_struct.tm_year + 1900;
    ts.month = tm_struct.tm_mon + 1;
    ts.day = tm_struct.tm_mday;
    ts.hour = tm_struct.tm_hour;
    ts.minute = tm_struct.tm_min;
    ts.second = tm_struct.tm_sec;
    ts.millisecond = tv.tv_usec / 1000;
#endif
    
    return ts;
}

std::string format_timestamp(const Timestamp& ts) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), 
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            ts.year, ts.month, ts.day,
            ts.hour, ts.minute, ts.second,
            ts.millisecond);
    return std::string(buffer);
}

void log_message(const char* message) {
    const char* log_filename = "counter_log.txt";
    
#ifdef _WIN32
    HANDLE hFile = CreateFileA(log_filename, 
                              GENERIC_WRITE, 
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    
    if (hFile == INVALID_HANDLE_VALUE) return;
    
    SetFilePointer(hFile, 0, NULL, FILE_END);
    
    OVERLAPPED overlapped = {0};
    LockFileEx(hFile, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped);
    
    DWORD bytes_written;
    std::string msg_str = std::string(message) + "\n";
    WriteFile(hFile, msg_str.c_str(), (DWORD)msg_str.length(), &bytes_written, NULL);
    
    UnlockFileEx(hFile, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(hFile);
#else
    FILE* f = fopen(log_filename, FOPEN_APPEND);
    if (!f) return;
    
    flock(fileno(f), LOCK_EX);
    fprintf(f, "%s\n", message);
    fflush(f);
    flock(fileno(f), LOCK_UN);
    fclose(f);
#endif
}

SharedMemory::SharedMemory() : handle(0), ptr(nullptr), is_owner(false) {
#ifdef _WIN32
    handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SharedCounter),
        SHM_NAME  
    );
    
    if (handle == NULL) {
        fprintf(stderr, "Ошибка создания разделяемой памяти: %lu\n", GetLastError());
        exit(1);
    }
    
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        is_owner = false;
    } else {
        is_owner = true;
    }
    
    ptr = (SharedCounter*)MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedCounter));
    if (!ptr) {
        CloseHandle(handle);
        fprintf(stderr, "Ошибка отображения памяти: %lu\n", GetLastError());
        exit(1);
    }
    
    if (is_owner) {
        memset(ptr, 0, sizeof(SharedCounter));
        ptr->value = 1;
        ptr->initialized = true;
    }
#else
    handle = shm_open(SHM_NAME, O_RDWR, 0666);
    
    if (handle == -1) {
        handle = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (handle == -1) {
            perror("shm_open");
            exit(1);
        }
        is_owner = true;
        if (ftruncate(handle, sizeof(SharedCounter)) == -1) {
            perror("ftruncate");
            exit(1);
        }
    } else {
        is_owner = false;
    }
    
    ptr = (SharedCounter*)mmap(NULL, sizeof(SharedCounter), 
                               PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    
    if (is_owner) {
        memset(ptr, 0, sizeof(SharedCounter));
        ptr->value = 1;
        ptr->initialized = true;
    }
    
    int wait_count = 0;
    while (!ptr->initialized && wait_count < 100) {
        usleep(10000);
        wait_count++;
    }
#endif
}

SharedMemory::~SharedMemory() {
    if (ptr) {
#ifdef _WIN32
        UnmapViewOfFile(ptr);
        if (handle) CloseHandle(handle);
#else
        munmap(ptr, sizeof(SharedCounter));
        if (handle != -1) {
            close(handle);
        }
#endif
    }
}

SharedCounter* SharedMemory::get() {
    return ptr;
}

Mutex::Mutex() : handle(0), is_owner(false) {
#ifdef _WIN32
    handle = CreateSemaphoreA(NULL, 1, 1, SEM_NAME); 
    if (handle == NULL) {
        fprintf(stderr, "Ошибка создания семафора: %lu\n", GetLastError());
        exit(1);
    }
    
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        is_owner = true;
    }
#else
    handle = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (handle == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }
#endif
}

Mutex::~Mutex() {
#ifdef _WIN32
    if (handle) CloseHandle(handle);
#else
    if (handle != SEM_FAILED) {
        sem_close(handle);
    }
#endif
}

void Mutex::lock() {
#ifdef _WIN32
    WaitForSingleObject(handle, INFINITE);
#else
    sem_wait(handle);
#endif
}

void Mutex::unlock() {
#ifdef _WIN32
    ReleaseSemaphore(handle, 1, NULL);
#else
    sem_post(handle);
#endif
}

LeaderElection::LeaderElection() : lock_file(0), is_leader(false), my_pid(get_current_pid()) {
    const char* lock_filename = "counter_leader.lock";
    
#ifdef _WIN32
    lock_file = CreateFileA(lock_filename,
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           NULL,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    
    if (lock_file != INVALID_HANDLE_VALUE) {
        is_leader = true;
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%lu", (unsigned long)my_pid);
        DWORD bytes_written;
        WriteFile(lock_file, pid_str, (DWORD)strlen(pid_str), &bytes_written, NULL);
    } else {
        HANDLE h = CreateFileA(lock_filename,
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
        
        if (h != INVALID_HANDLE_VALUE) {
            char pid_str[32] = {0};
            DWORD bytes_read;
            ReadFile(h, pid_str, sizeof(pid_str) - 1, &bytes_read, NULL);
            CloseHandle(h);
            
            unsigned long leader_pid = strtoul(pid_str, NULL, 10);
            if (leader_pid == 0 || !is_process_alive((platform_pid_t)leader_pid)) {
                CloseHandle(lock_file);
                DeleteFileA(lock_filename);
                
                lock_file = CreateFileA(lock_filename,
                                       GENERIC_READ | GENERIC_WRITE,
                                       0,
                                       NULL,
                                       CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL,
                                       NULL);
                
                if (lock_file != INVALID_HANDLE_VALUE) {
                    is_leader = true;
                    char pid_str[32];
                    snprintf(pid_str, sizeof(pid_str), "%lu", (unsigned long)my_pid);
                    DWORD bytes_written;
                    WriteFile(lock_file, pid_str, (DWORD)strlen(pid_str), &bytes_written, NULL);
                }
            }
        }
    }
#else
    int fd = open(lock_filename, O_CREAT | O_EXCL | O_RDWR, 0644);
    
    if (fd != -1) {
        is_leader = true;
        lock_file = fdopen(fd, "w");
        fprintf(lock_file, "%ld", (long)my_pid);
        fflush(lock_file);
    } else {
        FILE* f = fopen(lock_filename, "r");
        if (f) {
            long leader_pid = 0;
            fscanf(f, "%ld", &leader_pid);
            fclose(f);
            
            if (leader_pid == 0 || !is_process_alive((platform_pid_t)leader_pid)) {
                unlink(lock_filename);
                
                fd = open(lock_filename, O_CREAT | O_EXCL | O_RDWR, 0644);
                if (fd != -1) {
                    is_leader = true;
                    lock_file = fdopen(fd, "w");
                    fprintf(lock_file, "%ld", (long)my_pid);
                    fflush(lock_file);
                }
            }
        }
    }
#endif
}

LeaderElection::~LeaderElection() {
    if (is_leader) {
#ifdef _WIN32
        if (lock_file && lock_file != INVALID_HANDLE_VALUE) {
            CloseHandle(lock_file);
            DeleteFileA("counter_leader.lock");
        }
#else
        if (lock_file) {
            fclose(lock_file);
            unlink("counter_leader.lock");
        }
#endif
    }
}

bool LeaderElection::is_current_leader() {
    if (is_leader) {
        update_activity();
        return true;
    }
    return false;
}

void LeaderElection::update_activity() {
    if (!is_leader) return;
    
    extern SharedMemory* shared_mem;
    extern Mutex* global_mutex;
    
    if (shared_mem && global_mutex) {
        global_mutex->lock();
        SharedCounter* sc = shared_mem->get();
        sc->leader_pid = my_pid;
        sc->last_leader_activity = time(nullptr);
        global_mutex->unlock();
    }
}

platform_pid_t start_child_process(const char* mode) {
#ifdef _WIN32
    std::string exe_path = get_executable_path();
    char cmd_line[1024];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" %s", exe_path.c_str(), mode);
    
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    
    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, 
                       NULL, NULL, &si, &pi)) {
        return 0;
    }
    
    DWORD pid = pi.dwProcessId;
    
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    return pid;
#else
    pid_t pid = fork();
    if (pid == -1) return 0;
    
    if (pid == 0) {
        execl("/proc/self/exe", "/proc/self/exe", mode, (char*)nullptr);
        exit(1);
    }
    
    return pid;
#endif
}

bool is_process_alive(platform_pid_t pid) {
    if (pid == 0) return false;
    
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        return false;
    }
    
    DWORD exit_code;
    BOOL result = GetExitCodeProcess(hProcess, &exit_code);
    CloseHandle(hProcess);
    
    // STILL_ACTIVE = 259
    return (result && exit_code == STILL_ACTIVE);
#else
    return (kill((pid_t)pid, 0) == 0);
#endif
}
