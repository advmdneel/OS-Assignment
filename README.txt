
1) Open terminal and go to your folder
cd /mnt/c/Users/fuadm/Desktop/sudokuv2 // use you own path

2) Compile
gcc -o server server.c -lpthreadgcc -o client client.c

3) Run the server (Terminal 1)
./server

4) Run 3 clients in 3 more terminals (Terminal 2/3/4)
In each new terminal, go to the same folder then run:
Terminal 2:
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 0 Alice
Terminal 3:
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 1 Bob
Terminal 4:
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 2 Charlie

How to play
When you see “IT’S YOUR TURN”, type:
place R C N (R=row 1-9, C=col 1-9, N=number 1-9)
Example: place 5 7 2
Other useful commands:
status / grid / help / quit
If it doesn’t start
Make sure you start ./server first, then clients.
You need at least 3 clients for the game to start.


==================NEW==========================
Run this in WSL:
cd /mnt/c/Users/fuadm/Desktop/sudokuv2
make
./server

Then open 3 more WSL terminals and run (each terminal from the same folder):
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 0 Alice
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 1 Bob
cd /mnt/c/Users/fuadm/Desktop/sudokuv2./client 2 Charlie
