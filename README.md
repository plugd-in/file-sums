# file-sums
Sum three digit numbers in a file.

**Note**: The included "sums" binary was compiled and linked on a system using a different version of libc,
so this might need to be recompiled on other machines before use (use `gcc -o sums sums.c`).

About the Project
* The project I put together uses “argp” for command arguments, which means the command
includes a “--help” flag which displays the command documentation. There are more options
such as the “-c” or “--child-count” options.
* The project uses “epoll” for watching child pipes. I chose epoll because I wanted to learn about
epoll given its potential use in monitoring network sockets/connections. In hindsight, I could’ve
also used a single pipe given pipes are atomic in Unix for write operations of less than 512
bytes.
* I added some additional functionality that I thought would be cool, such as input/output file
arguments, and a block size argument, which allocates a number of children given a block size.
* The project also uses file “seeking” in order to allow children to jump to their block in a file.
* I also added fairly robust error handling. As an example, setting the number of children to an
extreme number (1000 children for example) will cause an error message such as “Error
creating pipes for child: Too many open files”.

------ 

__Note: Times recorded using the “time” unix command.__
 
|             | File 1        | File 2        | File 3        |
|-------------|---------------|---------------|---------------|
| 1 Process   | real 0m0.003s | real 0m0.005s | real 0m0.013s |
| 2 Processes | real 0m0.011s | real 0m0.003s | real 0m0.007s |
| 4 Processes | real 0m0.002s | real 0m0.003s | real 0m0.009s |
 

I’ve found that there is significant overhead to using fork for parallel computing. At some point, there
are diminishing, even reduced returns from increasing the number of children. This isn’t apparent based
on the run times recorded, however I found that increasing the number of children to more than the
number of cores on my CPU would cause a slower (or at least not faster) run time. I guess on a
significant scale, setting the number of children to 400 (which is approaching the point where too many
pipes are created) result in a “0m0.058s” runtime. Presumably, these results might differ on different
hardware as I got these results on a pentium-based laptop, which is severely limited in its capabilities.
