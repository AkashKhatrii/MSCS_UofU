#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <glob.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#define SH_TOK_BUFSIZE 64
#define SH_TOK_DELIM " \t\r\n\a"

char **history; 
int historyIndex = 0;

char *sh_read_line(void){
    char *line = NULL;
    size_t buffsize = 0;
    
    if(getline(&line, &buffsize, stdin) == -1){
        if(feof(stdin)){
            fprintf(stderr, "EOF\n");
            exit(EXIT_SUCCESS);
        }else{
            fprintf(stderr, "Value of errno: %d\n", errno);
            exit(EXIT_FAILURE);
        }
    }

    // printf("shr: %s\n", line);
    return line;
}


char **sh_split_line(char *line){
    int bufsize = SH_TOK_BUFSIZE;
    int position = 0;  // position in the buffer

    char **tokens = malloc(bufsize * sizeof(char *));
    char *token, **tokens_backup;

    if (!tokens){
        fprintf(stderr, "sh: allocation error\n");
        exit(EXIT_FAILURE);
    }

    token = strtok(line, SH_TOK_DELIM);

    while (token != NULL){
        tokens[position] = token;
        position ++;

        // checking if we have exceeded the current buffersize, if yes, allocate more memory
        if (position >= bufsize){
            bufsize += SH_TOK_BUFSIZE;
            tokens_backup = tokens;
            tokens = realloc(tokens, bufsize * sizeof(char *));

            if(!tokens){
                free(tokens_backup);
                fprintf(stderr, "sh: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, SH_TOK_DELIM);
    }

    tokens[position] = NULL;
    return tokens;
}

int sh_launch(char **args){

    // Variables for storing pipe, io, filenames, etc if any in the command;

    int is_pipe = 0;
    int is_glob = 0;
    int is_background = 0;
    int input_file = 0; // fd to store input file
    int output_file = 0; // fd to store output file
    int pipe_count = 0;
    int arg_count = 0; // to count the number of arguments passed
    int pipes[200]; // to store the location of pipes

    
    int k = 0;

    // check if the given args contain pipe, & or *:

    while (args[k] != NULL){
        arg_count ++;
        if (strstr(args[k], "|") != NULL){
            is_pipe = 1;
            pipes[pipe_count] = k;
            pipe_count ++;
        }
        if (strstr(args[k], "&") != NULL){
            is_background = 1;
        }
        if (strpbrk(args[k], "*?") != NULL){
            is_glob = 1;
        }
        k ++;
    }

    // Extra 1 - CD
    if (strstr(args[0], "cd") != NULL){
        chdir(args[1]);
        return 1;
    }


    // Extra 2 - HISTORY
    if (strstr(args[0], "history") != NULL){
        for(int i = 0; i < historyIndex; i ++){
            printf("%d  %s", i + 1, history[i]);
        }
        return 1;
    }

    // Extra 5 - BACKGROUND
    if (is_background){
        args[arg_count - 1] = NULL;
        int id = fork();

        if(id == 0){
            if (execvp(args[0], args) == -1){
                printf("Invalid command.\n");
                exit(1);
            };
        }
        else{
            wait(NULL);
            return 1;
        }

    }

    k = 0;
    
    // Part 1 and Part 2
    if (!is_pipe && !is_glob){  
        char **temp = malloc(sizeof(char *) * 64);
        int output = 0;
        int i = 0; // to iterate through args
        int filedesc = 0;
        int id = fork();

        if(id == 0){
            while(args[i] != NULL){
                if (strstr(args[i], ">") != NULL){
                    close(0);
                    open(args[i - 1], O_RDONLY);
                    int filedesc = open(args[i + 1], O_WRONLY | O_CREAT, 0666);
                    dup2(filedesc, 1);
                    close(filedesc);
                    i ++; // skipping the filename after >
                }else if(strstr(args[i], "<") != NULL){
                    close(0);
                    open(args[i + 1], O_RDONLY);
                    i ++; // skipping the filename after <
                }
                else{
                    temp[i] = args[i];
                }
                i ++;
            }

            temp[i + 1] = NULL;
            if (execvp(temp[0], temp) == -1){
                printf("Invalid command.\n");
                exit(1);
            };
        }
        else{
            wait(NULL);
            return 1;
        }

        return 1;
    }

    // Part 3
    if (is_pipe && !is_glob){
    pipes[pipe_count] = arg_count;
    int pid = fork();

    if (pid > 0){ // Parent
         int status;

         while(1){
            pid = waitpid(pid, &status, WNOHANG);
            if (pid != 0){
                break;
            }
         }
    }else if (pid == 0){ // Child
        int prev_read = 0; // initially the command reads from stdin

        int pos = 0; // indicates from which position to make new args

        for(int p = 0; p <= pipe_count; p ++){ // to iterate through all pipe locations and collect the arguments before that
            char **new_args = malloc(arg_count * sizeof(char *)); // args before that pipe

            if (new_args == NULL) {
                fprintf(stderr, "Memory allocation error\n");
                exit(EXIT_FAILURE);
            }

            int input_redir = 0;
            int count = pipes[p] - pos;

            for (int i = 0; i < count; i ++){
                new_args[i] = args[pos];
                pos++;

                if (p == 0 && strcmp(new_args[i], "<") == 0){ // bcz < command can be at the start
                    input_redir = 1;
                }
            }

            new_args[count] = NULL;
            pos ++;

            int fd[2]; 
            pipe(fd);

            int pid = fork();

            if (pid == 0){
                if(input_redir){
                    if (args[count - 1] != NULL){
                        input_file = open(args[count - 1], O_RDONLY);
                    }
                    dup2(input_file, 0);
                    close(input_file);
                    new_args[1] = NULL; // < -> NULL
                }

                if (p > 0){ // make the command read from the previous command if it is not the first pipe because command before first reads from stdin or a file
                    dup2(prev_read, 0);
                    close(prev_read);
                }

                if ( p < pipe_count){ // write to the pipe
                    dup2(fd[1], 1);
                    close(fd[1]);
                }

                if (execvp(new_args[0], new_args) == -1){
                    printf("Invalid command.\n");
                    exit(1);
                }
            }else if (pid > 0){
                if (p > 0){
                    close(prev_read);
                }
                close(fd[1]);
                prev_read = fd[0];
                int status;
                waitpid(pid, &status, 0);
                if(WEXITSTATUS(status) != 0){
                    printf("Invalid command.\n");
                    break;
                }
            }else{
                printf("Fork error.\n");
            }

        }
        }else{
            printf("Fork error.\n");
        }

        return 1;
    }

    

    // Extra 3 - GLOB
    if(is_glob){
        glob_t glob_result;
        int glob_flags = 0;
        int l = 0;

        char **new_args = malloc(100 * sizeof(char *));
        int na = 0;
        if (new_args == NULL) {
            fprintf(stderr, "Memory allocation error.\n");
            exit(EXIT_FAILURE);
        }
        while (args[l] != NULL) {
            if (strpbrk(args[l], "*?") != NULL) {
                if (glob(args[l], glob_flags, NULL, &glob_result) == 0) {
                    for (size_t j = 0; j < glob_result.gl_pathc; j++) {
                        new_args[na++] = strdup(glob_result.gl_pathv[j]);
                    }
                    globfree(&glob_result);
                }
            }else{
                    new_args[na++] = args[l];
                }
            l ++;
        }

        new_args[na] = NULL;

        if(execvp(new_args[0], new_args) == -1){
            printf("Invalid command.\n");
            exit(1);
        }
        return 1;
    }

}

int sh_execute(char **args)
{
  if (args[0] == NULL) {
    return 1;  // An empty command was entered.
  }
  return sh_launch(args);   // launch
}

void sh_loop(void){
    char *line;
    char **args;
    int status;

    do {
        printf("utsh$ ");
        fflush(stdout);
        line = sh_read_line();

        // Extra 2
        history[historyIndex] = strdup(line); // Adding command to history
        historyIndex ++;

        args = sh_split_line(line);

        // Extra 4 - ULTIPLE COMMANDS
        if(strstr(history[historyIndex - 1], ";") != NULL){ // if the last inserted command had a ; in it
            int i = 0;
            int new_command_index = 0;
            while(args[i] != NULL){
                if(strcmp(args[i],";") == 0){
                    args[i] = NULL;
                    sh_execute(args + new_command_index);
                    new_command_index = i + 1; // go to the next command
                }
                i++;
            }
            sh_execute(args + new_command_index);
        }
        else{
            status = sh_execute(args);
        }

        free(line);
        free(args);
    } while(status);
}
int main(int argc, char **argv)
{
    history = malloc(200 * sizeof(char *));

    if (history == NULL) {
        fprintf(stderr, "Memory allocation error\n");
        exit(EXIT_FAILURE);
    }

    sh_loop();
    return EXIT_SUCCESS;
}
