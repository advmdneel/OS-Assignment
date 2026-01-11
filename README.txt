================================================================================
        RACE TO 100 - Concurrent Networked Board Game
        Operating Systems Assignment - CSN6214
================================================================================

GAME DESCRIPTION
----------------
"Race to 100" is a multiplayer dice racing game for 3-5 players.

OBJECTIVE: Be the first player to reach 100 points!

RULES:
- On your turn, roll two dice (server generates random values)
- The sum of both dice is added to your score
- First player to reach 100 or more points WINS!
- SPECIAL RULE: If you roll snake eyes (1+1), your score resets to ZERO!

DEPLOYMENT MODE
---------------
Single-machine mode using:
- POSIX Shared Memory for game state
- Named Pipes (FIFOs) for client-server communication
- Process-shared mutexes for synchronization

COMPILATION
-----------
$ make                 # Compile both server and client
$ make clean           # Remove binaries and temp files

RUNNING THE GAME
----------------
1. Start the server (Terminal 1):
   $ ./server

2. Start clients in separate terminals (need 3-5 players):
   $ ./client 0 Alice      # Terminal 2
   $ ./client 1 Bob        # Terminal 3
   $ ./client 2 Charlie    # Terminal 4
   
   Or use make shortcuts:
   $ make client1          # Runs ./client 0 Player1
   $ make client2          # Runs ./client 1 Player2
   $ make client3          # Runs ./client 2 Player3

CLIENT COMMANDS
---------------
- roll (or r)    : Roll the dice on your turn
- status (or s)  : View current game state and all player scores
- help (or h)    : Display help information
- quit (or q)    : Leave the game

ARCHITECTURE OVERVIEW
---------------------
HYBRID CONCURRENCY MODEL:

1. MULTIPROCESSING (fork):
   - Server forks a child process for each connected client
   - SIGCHLD handler with waitpid() reaps zombie processes
   - Each child handles one player session

2. MULTITHREADING (pthreads):
   - Scheduler Thread: Manages Round Robin turn order
   - Logger Thread: Writes game events to game.log

3. SHARED MEMORY:
   - /game_state_shm: Stores game state, player data, turn info
   - /log_queue_shm: Circular buffer for log messages
   - /scores_shm: Persistent player win statistics

4. SYNCHRONIZATION:
   - Process-shared mutexes (PTHREAD_PROCESS_SHARED)
   - Condition variables for turn signaling
   - All critical sections properly protected

FILES GENERATED
---------------
- game.log    : Chronological log of all game events
- scores.txt  : Persistent storage of player win counts

SIGNAL HANDLING
---------------
- SIGINT (Ctrl+C): Graceful shutdown, saves scores
- SIGCHLD: Non-blocking zombie reaping
- SIGTERM: Graceful shutdown

SUPPORTED FEATURES
------------------
[✓] 3-5 player support
[✓] Hybrid concurrency (fork + pthreads)
[✓] Round Robin turn scheduling
[✓] Concurrent, thread-safe logging
[✓] Persistent score storage
[✓] Multi-game support (server resets after each game)
[✓] Disconnected player handling
[✓] Server-side randomness (dice rolls)

================================================================================
