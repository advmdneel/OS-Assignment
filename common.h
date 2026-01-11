#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

// Game Configuration
#define MIN_PLAYERS 3
#define MAX_PLAYERS 5
#define WINNING_SCORE 100
#define MAX_NAME_LEN 32
#define MAX_LOG_MSG 256
#define LOG_QUEUE_SIZE 100
#define SCORES_FILE "scores.txt"
#define LOG_FILE "game.log"
#define MAX_SCORES 100

// Shared Memory Names
#define SHM_GAME_STATE "/game_state_shm"
#define SHM_LOG_QUEUE "/log_queue_shm"
#define SHM_SCORES "/scores_shm"

// Named Pipe Base Path
#define PIPE_BASE "/tmp/game_pipe_"

// Player States
typedef enum {
    PLAYER_DISCONNECTED = 0,
    PLAYER_WAITING,
    PLAYER_ACTIVE,
    PLAYER_FINISHED
} PlayerState;

// Game States
typedef enum {
    GAME_WAITING_FOR_PLAYERS = 0,
    GAME_IN_PROGRESS,
    GAME_FINISHED
} GameState;

// Player Structure
typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    int score;
    PlayerState state;
    pid_t handler_pid;
} Player;

// Shared Game State
typedef struct {
    // Game info
    GameState game_state;
    int num_players;
    int current_turn;  // Index of current player
    int winner_id;     // -1 if no winner yet
    
    // Players
    Player players[MAX_PLAYERS];
    
    // Synchronization (process-shared)
    pthread_mutex_t game_mutex;
    pthread_cond_t turn_cond;
    pthread_mutex_t turn_mutex;
    
    // Flags
    int game_reset_requested;
    int server_shutdown;
} SharedGameState;

// Log Entry
typedef struct {
    char message[MAX_LOG_MSG];
    time_t timestamp;
} LogEntry;

// Log Queue (circular buffer)
typedef struct {
    LogEntry entries[LOG_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    int shutdown;
} LogQueue;

// Score Entry for Persistence
typedef struct {
    char name[MAX_NAME_LEN];
    int wins;
} ScoreEntry;

// Scores Structure
typedef struct {
    ScoreEntry entries[MAX_SCORES];
    int count;
    pthread_mutex_t mutex;
} SharedScores;

// Message Types for Client-Server Communication
typedef enum {
    MSG_JOIN = 1,
    MSG_ROLL,
    MSG_QUIT,
    MSG_GAME_STATE,
    MSG_YOUR_TURN,
    MSG_ROLL_RESULT,
    MSG_GAME_OVER,
    MSG_WAIT,
    MSG_ERROR,
    MSG_PLAYER_JOINED,
    MSG_PLAYER_LEFT,
    MSG_GAME_START
} MessageType;

// Client-Server Message
typedef struct {
    MessageType type;
    int player_id;
    char player_name[MAX_NAME_LEN];
    int value;  // For roll results, scores, etc.
    char text[MAX_LOG_MSG];
} GameMessage;

// Function Prototypes
void init_process_shared_mutex(pthread_mutex_t *mutex);
void init_process_shared_cond(pthread_cond_t *cond);

#endif // COMMON_H
