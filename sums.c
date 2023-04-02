/***
The APACHE License (APACHE)

Copyright (c) 2023 Reynaldo Bontje. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
***/

#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <argp.h>
#include <assert.h>
#include <stdbool.h>
#include <error.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>


/***
 * Project Structure:
 *  * Argp
 *    * Used for handling command-line arguments. Also has a built-in --help command.
 *      * --block-size
 *      * --child-count or -c 
 *      * --input-file or -i 
 *      * --output-file or -o 
 *  * Child process structures.
 *    * Structures/functions used for creating children.
 *  * "epoll" polling
 *    * Watches the pipes and notifies of writes by children.
 * 
 * Together, the three pieces (argp, child process handling, and epoll)
 * make up the core of the program.
 ***/


/***
 *
 * Argp (Command-line Argument) Parsing Section
 *
 */

// Program information for --help output.
const char * argp_program_version = "File Summer";
static char doc[] = "A program for summation.";
static char args_doc[] = "";

// "Keys" for the command line arguments/flags.
enum OPTION_KEYS {
  INPUT_FILE = 'i', // -i <.dat file>
  OUTPUT_FILE = 'o', // -o <output> default to "-" for stdout
  CHILD_COUNT = 'c', // -c <number of children>
  // (Child Count) = (File Byte Count)/(BLOCK_SIZE)
  BLOCK_SIZE = 256 // No short option "--block-size".
};

static struct argp_option options[] = {
  // For the --block-size argument.
  {
    "block-size",
    BLOCK_SIZE,
    "SIZE",
    ARGP_LONG_ONLY,
    "Block size for which children should be"
    " allocated for. Should not be used with '--children'."
  },
  // For the --input or -i argument.
  {
    "input",
    INPUT_FILE,
    "FILE",
    0,
    "The file for which sums should be calculated. "
    "Defaults to using standard input. Standard input only"
    " allows a single child to process the sums."
  },
  // For the --output or -o option.
  {
    "output",
    OUTPUT_FILE,
    "FILE",
    0,
    "Where to put the results. Defaults"
    " to \"-\", sending it to stdout"
  },
  // For the --child-count or -c option.
  {
    "child-count",
    CHILD_COUNT,
    "COUNT",
    0,
    "The number of children to spawn, with n >= 1. "
    "Should not be used with '--block-size'."},
  {0}
};

// Option/argument structure
//  ; the argp_parse function will construct/modify this
//  ; and it will be available globally.
//  ; 
//  ; It will hold the user arguments.
struct program_options {
  char * input_file;
  FILE * output_file;
  u_int16_t child_count;
  u_int64_t block_size;
  // Keep track of whether block/child args are used.
  bool _used_block;
  bool _used_child;

  struct stat _stat_buf;
};

// Default values for options.
struct program_options program_options = {
  // "-" will mean that standard input (stdin) should be used.
  .input_file = "-",
  .output_file = NULL,
  .child_count = 1,
  // block_size of one indicates the block size should be
  // based on the child_count
  .block_size = 0,
  ._used_block = false,
  ._used_child = false
};

/***
 * Parses command-line arguments, adding values to
 * the program_options global.
 */
static error_t parse_opt (int key, char * arg, struct argp_state * state) {
  struct program_options * arguments = state->input; // Should refer to the program_options global.
  assert(arguments == &program_options);
  switch (key) {
    case BLOCK_SIZE:
      // Doesn't detect errors, unfortunately.
      // consider using strtol family of functions.
      //
      // However, it returns a "0" on error, which we
      // will use to indicate no block size.

      // Should not be used with --child-count
      if ( arguments->_used_child )
        return EINVAL;
      arguments->block_size = atoi(arg);
      arguments->_used_block = true;
      break;
    case INPUT_FILE:
      // Opens the file for reading OR uses stdin.
      arguments->input_file = arg;
      break;
    case OUTPUT_FILE:
      // Opens the file for reading or uses stdout.
      if ( strcmp("-", arg) == 0 )
        arguments->output_file = stdout;
      else
        arguments->output_file = fopen(arg, "w");
      break;
    case CHILD_COUNT:
      // Doesn't detect errors, unfortunately.
      // consider using strtol family of functions.
      //
      // However, it returns a "0" on error, which we
      // will use to indicate no block size.

      // Should not be used with --child-count
      if ( arguments->_used_block )
        return EINVAL;
      arguments->child_count = atoi(arg);
      arguments->_used_child = true;
      // Should be more than zero children.
      if ( arguments->child_count <= 0 )
        return EINVAL;
      break;
    default:
      // An unknown argument was passed along.
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

// Defines the basic structure of how arguments should
// be parsed by argp.
struct argp argp = {
    .options = options,
    .parser = parse_opt,
    .args_doc = args_doc,
    .doc = doc
};

// Proxy function that calls argp_parse.
// Could be done in the "main" function,
// but it's useful to keep doing things after
// arguments have been parsed.
void handle_options (int argc, char ** argv) {
  error_t result = argp_parse(&argp, argc, argv, 0, 0, &program_options);

  if ( result ) // If there was an error:
    // Exit with an error.
    error(EXIT_FAILURE, result, "Error parsing agruments");
  // If the user didn't specify an output file,
  // use standard output.
  if ( program_options.output_file == NULL )
    program_options.output_file = stdout;
}

/***
 *
 * Child Handling Section
 *
 */

// This will be written to the pipe by the children.
struct child_result {
  u_int16_t child_num;
  u_int64_t sum;
};

// Basic arguments/variables needed by the children.
struct child_info {
  int fds[2]; // Target for pipe.
  u_int64_t seek_to; // Start in file.
  u_int64_t read_to; // Where the child should stop reading.
  u_int16_t child_num; // For identification.
  struct epoll_event event_structure;
  struct child_result result;
};

// Pre-definition, so child_list can reference itself.
struct child_list;

// Child list structure.
struct child_list {
  struct child_list * next;
  struct child_info child_info;
};

/***
* open_and_seek_to: Opens a file, seeks to a given position
* and then returns the opened/seeked stream.
*
* `path` (char *): The path to the file to open.
* `position` (u_int64_t): The position within the file to seek to.
*/
FILE * open_and_seek_to (char * path, u_int64_t position) {
  FILE * file = fopen(path, "r");
  fseek(file, position, SEEK_SET);
  return file;
}

/***
* handle_stdin: Handles the case where the input "file"
* is the standard input.
*/
void handle_stdin (FILE * file, int fd, u_int16_t child_num) {
  // Where the results will be written to.
  struct child_result result = {0};
  // Sets the child_num so the parent will
  // know which child returned a result.
  result.child_num = child_num;

  // These will hold the digits of each number/line.
  // An extra is used for the null terminator.
  char buf[4] = {0};
  // Will hold the character currently read from the stream.
  int c;
  // Will hold how many digits have been read on the current line.
  // Any more than three digits/characters will be ignored, preventing
  // an overflow error.
  int c_count = 0;
  // Loop that reads to the end of the input stream,
  // reading/summing digits along the way.
  while ( ( c = fgetc(file) ) != EOF ) {
    // Reads the first three digits per line into the "buf"
    // array.
    if ( c_count < 3 && isdigit(c) ) {
      buf[c_count] = c;
      c_count += 1;
    }
    // When three digits have been acquired,
    // use atoi to turn them into an int.
    if ( c_count >= 3 ) {
      // Add the current number to the total sum.
      result.sum += atoi(buf);
      // Reset the current number.
      c_count = 0;
    }
  }
 
  // Write the result to the pipe.
  write(fd, &result, sizeof(result));
  // Close the pipe.
  close(fd);
}

/***
* handle_file: Handle the case where a file/path is passed along
*   as the input file. This means that some "seeking" logic needs to be used.
*/
void handle_file (FILE * file, int fd, u_int16_t child_num, u_int64_t seek_to, u_int64_t read_to) {
  // Keep track of currentposition in file, so the child
  // knows when to stop reading numbers.
  u_int64_t pos = seek_to;
  // Will be passed along to the parent.
  struct child_result result = {0};
  // So the parent knows which child returned results.
  result.child_num = child_num;

  // Hold the digits
  char buf[4] = {0};
  // Hold the current character.
  int c;
  // Hold the current digit place (hundred, ten, one).
  int c_count = 0;
  // Loop until the child reaches the end of their block,
  // or until an EOF.
  while ( pos <= read_to && ( c = fgetc(file) ) != EOF ) {
    // read digits into buf
    if ( c_count < 3 && isdigit(c) ) {
      buf[c_count] = c;
      c_count += 1;
    }
    // Add the digit results into the sum.
    if ( c_count >= 3 ) {
      result.sum += atoi(buf);
      c_count = 0;
    }
    pos += 1;
  }
 
  // Send the results to the parent.
  write(fd, &result, sizeof(result));
  close(fd);
}

// The children will end up here after forking.
int child_handler (struct child_info child_info) {
  // Will hold whether standard input is being used.
  bool is_stdin = false;
  // Will hold the input file.
  FILE * file;
  // Where to stop.
  u_int64_t read_to;

  // Check whther the standard input should be used.
  if ( strcmp(program_options.input_file, "-") == 0 ) {
    is_stdin = true;
  }

  // -1 indicates the file should be read to the end.
  if ( !is_stdin && child_info.read_to == -1 )
    read_to = program_options._stat_buf.st_size;
  else if ( !is_stdin )
    read_to = child_info.read_to;

  // Set the "file" variable to either the passed file
  // or the standard input.
  if ( is_stdin )
    file = stdin;
  else
    // Open the file for reading and seek to the
    // start of the block that this child is responsible for.
    file = open_and_seek_to(program_options.input_file, child_info.seek_to);

  // Handle reading/summing based on if a file
  // or standard input is being used as input.
  if ( is_stdin ) 
    handle_stdin(file, child_info.fds[1], child_info.child_num);
  else
    handle_file(file, child_info.fds[1], child_info.child_num, child_info.seek_to, read_to);
  
  // Will cause the child to exit with a success status code.
  return 0;
}

// Creates a child process and child information.
struct child_list * add_child (int epoll_fd, struct child_list * list_head, int seek_to, int read_to) {
  // Allocate new child memory/info.
  struct child_list * new_child = malloc(sizeof(struct child_list));
  // Will eventually hold which child
  // number this child is.
  int child_num = 0;
  // Will be used for looping to the last node
  // in the list.
  struct child_list * current = list_head;
  // Will hold the result from "fork" system call.
  pid_t fork_result;

  // Create child pipes.
  int result = pipe(new_child->child_info.fds);
  if ( result == -1 ) { // pipe returns -1 to indicate an error.
    perror("Error creating pipes for child");
    exit(1);
  }
  // Go to end of list.
  while ( current->next != NULL ) {
    child_num += 1;
    current = current->next;
  }
  
  // Set child properties.
  new_child->child_info.child_num = child_num;

  // Set child block boundaries.
  new_child->child_info.seek_to = seek_to;
  new_child->child_info.read_to = read_to;

  // Fork child process.
  fork_result = fork();

  if ( fork_result == -1 ) { // An error occured.
    perror("Error forking child.");
    exit(1);
  } else if ( fork_result == 0 ) { // Child:
    // Call child_handler, exiting with the return value of that
    // function.
    exit(child_handler(new_child->child_info));
  }

  // Register pipes with epoll so the parent process knows when
  // the child writes to the pipe.
  new_child->child_info.event_structure.events = EPOLLIN;
  new_child->child_info.event_structure.data.fd = \
    new_child->child_info.fds[0];

  epoll_ctl(
      epoll_fd,
      EPOLL_CTL_ADD,
      new_child->child_info.fds[0],
      &new_child->child_info.event_structure);

  // Add the child to the linked list.
  current->next = new_child;

  // Return the child node in the linked list.
  return new_child;
}

int main (int argc, char ** argv) {
  // Will hold the final sum.
  long final_sum = 0;
  // Will hold the number of children that have yet to
  // return their sum.
  u_int16_t waiting_for = 0;

  // Use epoll to watch file descriptors.
  // Note that the argument "1" is discarded and it doesn't
  // matter what it is set to.
  int epoll_fd = epoll_create(1);
  // The list head for children.
  struct child_list list_head = {0};

  if ( epoll_fd == -1) { // epoll_create returns -1 on error.
    perror("epoll create");
    exit(EXIT_FAILURE);
  }

  // Handle/process the arguments/options.
  // This will handle the user arguments
  // and fill in the program_options global.
  handle_options(argc, argv);

  // Will hold the output file,
  // including the standard output if that's what's desired.
  FILE * file;
  // Will hold input file information.
  struct stat stat_buf;
  // Will hold the return value of a stat operation.
  // Used to detect errors.
  int stat_result;

  if ( strcmp("-", program_options.input_file) == 0 ) {
    // Warnings because standard input is not seekable.
    if (program_options.block_size) {
      fprintf(
        stderr,
        "Warn: using stdin... ignoring block size %lu.\n",
        program_options.block_size
      );
      program_options.block_size = 0;
    }
    if ( program_options.child_count > 1 ) {
      fprintf(
        stderr,
        "Warn: using stdin... ignoring child count %d.\n",
        program_options.child_count
      );
      program_options.child_count = 1;
    } 
    file = stdin;
  } else {
    // Get file information/stats.
    stat_result = stat(program_options.input_file, &program_options._stat_buf);
    // Stat error.
    if ( stat_result == -1 ) {
      perror("Error checking input file");
      exit(EXIT_FAILURE);
    }
    fprintf(program_options.output_file, "File size: %lu\n", program_options._stat_buf.st_size);
    // Flush the writes output file.
    // Without this, there was a problem with the above printf repeating.
    fflush(program_options.output_file);
  }

  // Handles the case where block_size or child_count are used.
  if ( program_options.block_size > 0 ) {
    // Set how many children should be spawned given a block size.
    program_options.child_count = program_options._stat_buf.st_size / program_options.block_size;
  } else {
    // Divide the files into blocks for the children.
    program_options.block_size = program_options._stat_buf.st_size / program_options.child_count;
  }

  // Set how many children will need to be waited on.
  waiting_for = program_options.child_count;

  // Create the children.
  for ( int i = 0; i < program_options.child_count; i++ ) {
    // Set the block the child will be responsible for.
    u_int64_t seek_to = i * program_options.block_size;
    u_int64_t read_to;
    // The last child will read to the end of the file/stream.
    // The add_child function interprets -1 as
    // an indication that the child should read to the end
    // of the file.
    if ( (i + 1) == program_options.child_count )
      read_to = -1;
    else // Otherwise, the child should read to just before the start of the next block.
     read_to = (i+1)*program_options.block_size - 1;
    // Add the child with the given block boundaries.
    add_child(epoll_fd, &list_head, seek_to, read_to);
  }

  // Keep polling for pipe output until all the children
  // have returned some results.
  while (waiting_for > 0) {
    // If an event occurs, it'll be put into
    // this structure:
    struct epoll_event ev;
    // This call will block until a pipe
    // is readable:
    epoll_wait(epoll_fd, &ev, 1, -1);

    // Note that the above call hasn't set a timeout
    // so it might get indefinitely stuck if one of the children
    // fails.

    // Read the output sent by the child into "result"
    struct child_result result;
    ssize_t bytes = read(ev.data.fd, &result, sizeof(struct child_result));

    // If the message is meaningful, it's probably expected output:
    if ( bytes > 0 ) {
      // Print the Child Number and the sum the child calculated.
      fprintf(program_options.output_file, "Child %d Sum: %lu\n", result.child_num, result.sum);
      // Add the child's result to the final_sum.
      final_sum += result.sum;
      // Wait for one less child.
      waiting_for -= 1;
      // Stop polling for events on this child.
      epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ev.data.fd, NULL);
    } else { // Something unexpected. Stop polling the child.
      epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ev.data.fd, NULL);
    }
  }

  // This should be after children have returned results.
  // Output the final sum:
  fprintf(program_options.output_file, "Final Sum: %lu\n", final_sum);

  return 0;
}
