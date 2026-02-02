# Collaborative Sudoku Game

A multiplayer Sudoku game for Minix/Mini OS where 3-5 players compete to solve a shared puzzle and earn the highest score.

## Features

- **Multiplayer Support**: 3-5 players can join and compete simultaneously
- **Configurable Grid Size**: Supports 4x4, 6x6, 9x9, 12x12, and 16x16 grids
- **Turn-Based Gameplay**: Round-robin turn system ensures fair play
- **Real-Time Updates**: All players see moves as they happen
- **Scoring System**: +10 points for correct placements, -5 for wrong ones
- **Game Logging**: Complete game history saved to `game_log.txt`
- **Player Statistics**: Persistent stats tracked in `player_stats.txt`


## Compilation

cd /mnt/c/Users/fuadm/Desktop/sudokuv2 //Your file path
make
./server

Then open 3 more terminals and run (each terminal from the same folder):
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 0 Alice
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 1 Bob
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 2 Charlie

## Running the Game


## Configuring Grid Size

To change the grid size, modify these constants in **BOTH** `server.c` and `client.c`:

#define GRID_SIZE 9        // Grid dimension
#define BOX_ROWS  3        // Number of rows per box
#define BOX_COLS  3        // Number of columns per box  
#define BOX_SIZE  3        // For square boxes
#define MAX_NUM   GRID_SIZE


### Grid Size Reference Table

| Grid  | GRID_SIZE | BOX_ROWS | BOX_COLS | BOX_SIZE |
|-------|-----------|----------|----------|----------|
| 4x4   | 4         | 2        | 2        | 2        |
| 6x6   | 6         | 2        | 3        | 2        |
| 9x9   | 9         | 3        | 3        | 3        |
| 12x12 | 12        | 3        | 4        | 3        |
| 16x16 | 16        | 4        | 4        | 4        |

**Important**: `GRID_SIZE` must equal `BOX_ROWS × BOX_COLS`

## Game Commands

| Command | Description |
|---------|-------------|
| `place R C N` | Place number N at row R, column C |
| `p R C N` | Short form of place |
| `status` | View current game state and scores |
| `grid` | Display the Sudoku grid |
| `help` | Show help message |
| `quit` | Leave the game |

### Example

place 3 5 7    # Places 7 at row 3, column 5
p 1 2 4        # Places 4 at row 1, column 2


## Grid Legend

| Symbol | Meaning |
|--------|---------|
| `N` | Fixed number (given in puzzle) |
| `*N` | Number placed by you |
| `+N` | Number placed by another player |
| `.` | Empty cell |

## Scoring

- **Correct placement**: +10 points
- **Wrong placement**: -5 points (cell remains empty)

The player with the highest score when the puzzle is complete wins.

## Architecture

┌─────────────────────────────────────────────────────────┐
│                      SERVER                             │
│  ┌─────────────────────────────────────────────────┐    │
│  │           Shared Memory (Game State)            │    │
│  │  - Sudoku Grid    - Player Info                 │    │
│  │  - Scores         - Turn Management             │    │
│  └─────────────────────────────────────────────────┘    │
│                          │                              │
│     ┌────────────────────┼────────────────────┐         │
│     │                    │                    │         │
│  ┌──▼──┐              ┌──▼──┐              ┌──▼──┐      │
│  │Child│              │Child│              │Child│      │
│  │Proc │              │Proc │              │Proc │      │
│  │  0  │              │  1  │              │  2  │      │
│  └──┬──┘              └──┬──┘              └──┬──┘      │
└─────┼───────────────────┼───────────────────┼───────────┘
      │ Named Pipes       │                   │
      ▼                   ▼                   ▼
 ┌────────┐          ┌────────┐          ┌────────┐
 │Client 0│          │Client 1│          │Client 2│
 └────────┘          └────────┘          └────────┘

### Components

- **Server Process**: Manages game state, validates moves, coordinates turns
- **Child Processes**: One per player, handles communication via named pipes
- **Shared Memory**: Stores game state accessible by all server processes
- **Named Pipes**: Bidirectional communication between server and clients
- **Spin Locks**: Ensures thread-safe access to shared game state

## Files Generated

| File | Description |
`game_log.txt` | Complete game history with timestamps |
`player_stats.txt` | Persistent player statistics (wins, games played, etc.) |
`/tmp/sudoku_pipe_*` | Named pipes for IPC (cleaned up on exit) |

## Troubleshooting

### "Server not running" error
Make sure the server is started before connecting clients.


### Client not receiving updates
Ensure the client and server are compiled with the same `GRID_SIZE` settings.

## Game Flow

1. Server starts and initializes shared memory
2. Players connect and enter their names
3. Game waits until minimum 3 players join
4. Puzzle is generated based on difficulty
5. Players take turns placing numbers
6. Server validates each move and updates scores
7. All players are notified when the game ends
8. Winner is announced with final scores
9. Statistics are saved to file

