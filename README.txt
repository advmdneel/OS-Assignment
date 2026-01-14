================================================================================
        COLLABORATIVE SUDOKU - Concurrent Networked Board Game
        Operating Systems Assignment
        (Minix 3 / Mini OS Compatible Version)
================================================================================

GAME DESCRIPTION
----------------
"Collaborative Sudoku" is a multiplayer puzzle game for 3-5 players.

OBJECTIVE: Earn the most points by correctly filling in Sudoku cells!

RULES:
- Players take turns placing numbers (1-9) in empty cells
- Each correct placement earns +10 points
- Each wrong placement costs -5 points (cell stays empty)
- Game ends when the puzzle is complete
- Player with the highest score WINS!

SUDOKU RULES REMINDER:
- Each row must contain digits 1-9 (no repeats)
- Each column must contain digits 1-9 (no repeats)
- Each 3x3 box must contain digits 1-9 (no repeats)

FILES INCLUDED
--------------
- server.c          : Game server (self-contained, no headers needed)
- client.c          : Game client (self-contained, no headers needed)
- README.txt        : This documentation file

FILES GENERATED AT RUNTIME
--------------------------
- sudoku_game.log   : Chronological log of all game events
- sudoku_scores.txt : Persistent storage of player statistics

COMPILATION
-----------
Linux / Most Unix systems:
    gcc -o server server.c -lpthread
    gcc -o client client.c

Minix 3:
    cc -o server server.c -lpthread
    cc -o client client.c

RUNNING THE GAME
----------------
1. Start the server (Terminal 1):
   $ ./server

2. Start clients in separate terminals (need 3-5 players):
   $ ./client 0 Alice      # Terminal 2 - Player slot 0
   $ ./client 1 Bob        # Terminal 3 - Player slot 1
   $ ./client 2 Charlie    # Terminal 4 - Player slot 2
   $ ./client 3 Diana      # Terminal 5 - Player slot 3 (optional)
   $ ./client 4 Eve        # Terminal 6 - Player slot 4 (optional)

CLIENT COMMANDS
---------------
  place R C N    Place number N at row R, column C
                 Example: "place 3 5 7" puts 7 at row 3, col 5
  status         View current game state and all player scores
  grid           Display the Sudoku grid
  help           Display help information
  quit           Leave the game

GRID DISPLAY LEGEND
-------------------
  5      Fixed cell (given in puzzle, cannot be changed)
  *5     Cell YOU placed
  +5     Cell placed by another player
  .      Empty cell (can be filled)

ARCHITECTURE OVERVIEW
---------------------
HYBRID CONCURRENCY MODEL:

1. MULTIPROCESSING (fork):
   - Server forks a child process for each connected client
   - SIGCHLD handler with waitpid() reaps zombie processes

2. MULTITHREADING (pthreads):
   - Scheduler Thread: Manages Round Robin turn order
   - Logger Thread: Writes game events to sudoku_game.log

3. SHARED MEMORY (System V IPC):
   - Game state, Sudoku grid, player data
   - Circular buffer for log messages
   - Persistent player statistics

4. SYNCHRONIZATION:
   - Atomic spinlocks for cross-process synchronization
   - Polling-based turn notification

COMPATIBILITY
-------------
Designed to run on:
- Minix 3
- Linux (all distributions)
- BSD systems
- Other POSIX-compliant systems

Key compatibility features:
- System V IPC (shmget/shmat) instead of POSIX shm_open
- Atomic spinlocks instead of process-shared pthread mutexes
- Polling instead of condition variables
- ASCII-only output (no Unicode)

SCORING SYSTEM
--------------
  Correct placement: +10 points
  Wrong placement:   -5 points

The player with the highest score when the puzzle is complete wins.

TROUBLESHOOTING
---------------
If shared memory persists after a crash, remove manually:
    ipcrm -M 0x5355444F
    ipcrm -M 0x4C4F4753
    ipcrm -M 0x53434F52

To check existing shared memory segments:
    ipcs -m

SIGNAL HANDLING
---------------
- SIGINT (Ctrl+C): Graceful shutdown, saves scores
- SIGCHLD: Non-blocking zombie reaping

================================================================================
