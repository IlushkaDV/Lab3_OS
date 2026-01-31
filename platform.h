#ifndef PLATFORM_H
#define PLATFORM_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <string>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <process.h>
    #include <io.h>
    #include <fcntl.h>
    #include <sys/timeb.h>
    
    typedef DWORD platform_pid_t;
    typedef HANDLE platform_shm_t;
    typedef HANDLE platform_sem_t;
    typedef HANDLE platform_file_t;
    
    #define FOPEN_APPEND "a"
    
    inline platform_pid_t get_current_pid() { return GetCurrentProcessId(); }
    inline void sleep_ms(int ms) { Sleep(ms); }
    
    inline std::string get_executable_path() {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        return std::string(path);
    }
    
    #define SHM_NAME "CounterSharedMemory"
    #define SEM_NAME "CounterMutex"
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <semaphore.h>
    #include <sys/stat.h>
    #include <sys/file.h>
    #include <signal.h>
    #include <sys/time.h>
    #include <errno.h>
    
    typedef pid_t platform_pid_t;
    typedef int platform_shm_t;
    typedef sem_t* platform_sem_t;
    typedef FILE* platform_file_t;
    
    #define FOPEN_APPEND "a"
    
    inline platform_pid_t get_current_pid() { return getpid(); }
    inline void sleep_ms(int ms) { usleep(ms * 1000); }
    inline const char* get_executable_path() { return "/proc/self/exe"; }
    
    #define SHM_NAME "/counter_shared_mem"
    #define SEM_NAME "/counter_mutex"
#endif

struct Timestamp {
    int year, month, day;
    int hour, minute, second, millisecond;
};

Timestamp get_current_timestamp();
std::string format_timestamp(const Timestamp& ts);
void log_message(const char* message);

struct SharedCounter {
    int64_t value;
    platform_pid_t child1_pid;
    platform_pid_t child2_pid;
    time_t child1_start_time;
    time_t child2_start_time;
    platform_pid_t leader_pid;
    time_t last_leader_activity;
    bool initialized;
};

class SharedMemory {
private:
    platform_shm_t handle;
    SharedCounter* ptr;
    bool is_owner;
    
public:
    SharedMemory();
    ~SharedMemory();
    SharedCounter* get();
};

class Mutex {
private:
    platform_sem_t handle;
    bool is_owner;
    
public:
    Mutex();
    ~Mutex();
    void lock();
    void unlock();
};

class LeaderElection {
private:
    platform_file_t lock_file;
    bool is_leader;
    platform_pid_t my_pid;
    
public:
    LeaderElection();
    ~LeaderElection();
    bool is_current_leader();
    void update_activity();
};

platform_pid_t start_child_process(const char* mode);
bool is_process_alive(platform_pid_t pid);

#endif // PLATFORM_H