#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <mush.h>

#define PROMPT "8-P"
#define READ_END 0
#define WRITE_END 1
#define PERMS S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

#define PIPE_FDS 2

static int isSigint = 0;

void usage()
{
    fprintf(stderr,"usage: mush2 [filename]\n");
    exit(EXIT_FAILURE);
}

void err_sys(const char* caller,const char* func)
{
    fprintf(stderr,"%s: ",caller);
    perror(func);
    exit(EXIT_FAILURE);
}


void closePipe(int *fd)
{
    if(!fd)
        return;

    if(fd[READ_END] != STDIN_FILENO)
        close(fd[READ_END]);
    if(fd[WRITE_END] != STDOUT_FILENO)
        close(fd[WRITE_END]);
}


void closePipeArr(int** pipeArr, int num)
{
    int i;

    for(i=0; i<num; ++i)
        closePipe(pipeArr[i]);
}

void exec_cd(struct clstage* curStage)
{
    char* path = NULL;
    struct passwd* pwd;

    /* check if argument is present */
    if(curStage->argv[1] != NULL)
        path = curStage->argv[1];
    else /*if not, find home dir */
    {
        /* check environment variable */
        if((path = getenv("HOME")) == NULL)
        {
            /* if HOME is not specified, look up the user's pwd entry*/
            if((pwd = getpwuid(getuid())) == NULL)
            {
                fprintf(stderr,"unable to determine home directory: ");
                perror("getpwuid");
                return;
            }
        
            /*if cannot determine home dir, return */
            if((path = pwd->pw_dir) == NULL)
            {
                fprintf(stderr,"unable to determine home directory\n");
                return;
            }
        }         
    }

    if(chdir(path) < 0)
        perror(path);
}

int isBuiltIn(struct clstage* curStage)
{
    /* list of supported built-in commands */
    char* BuiltInCmd[] =  {"cd"}; 
    /* list of execution functions for each built-in command*/
    void (*BuiltInCmd_exec[])(struct clstage*) = {&exec_cd};
    
    int i,n;
    char* cmd = curStage->argv[0];

    n = sizeof(BuiltInCmd)/sizeof(BuiltInCmd[0]);
    for(i=0; i < n; ++i)
        /* if current cmd is a built-in command */
        if(strcmp(BuiltInCmd[i], cmd) == 0)
        {
            /* execute it and return */
            (*BuiltInCmd_exec[i])(curStage);    
            return 1;
        }

    return 0;    
}

void handler(int signum)
{
    isSigint = 1;
    printf("\n");
    fflush(stdout);
}

void freePipeArr(int*** pipeArr, int num)
{
    int i=0; 
    
    for(i=0; i<num; ++i)
        if((*pipeArr)[i])
            free((*pipeArr)[i]);
    
    free(*pipeArr);    
}

int** iniPipeArr(int num)
{
    int **pipeArr;
    int i = 0;

    /* allocate memory & check error */
    if((pipeArr = malloc(num*sizeof(int*))) == NULL)
    {
        perror("iniPipeAr: malloc");
        return NULL;
    }

    /* each pipe allocate an array of length PIPE_FDS which is 2 */
    for(i=0; i<num; ++i)
    {
        if((pipeArr[i] = malloc(PIPE_FDS*sizeof(int))) == NULL)  
        {
            perror("iniPipeArr: malloc");
            freePipeArr(&pipeArr, i-1);
            return NULL;
        }

        /* create a pipe */
        if(pipe(pipeArr[i]) < 0)
        {
            perror("iniPipeArr pipe");
            freePipeArr(&pipeArr,i);
            return NULL;
        }
    }

    return pipeArr;
}


void killChild(pid_t *childArr, int n)
{
    int i = 0;

    for(i=0; i<n; ++i)
    {
        /* kill ith child, if kill return any error other than 
         * notify that the child does not exist, print error */ 
        if(kill(childArr[i],0) < 0 && errno != ESRCH)
            perror("kill");
    }
}

/* return -1 on error, 0 on success */
int forkChild(pipeline pl, int *cntChild, int** pipeArr,
               pid_t* childArr, sigset_t *set)
{
    struct clstage* curstage;
    pid_t pid;
    int infd, outfd, n, i;

    /* num of stages */
    n = pl->length;

    /* go through each stage and fork child */
    for(i=0; i<n; ++i)    
    {
        curstage = &pl->stage[i];
    
        if(isBuiltIn(curstage))
            continue;

        /* create new child process */
        if((pid = fork()) < 0)
        {
            perror("forkChild: fork");
            return -1;
        } 
    
        /* child process if fork return 0 */
        if(pid == 0)
        {
            /* save current child pid in childArr */
            childArr[(*cntChild)] = pid;
                   
            /* first stage reads from stdin */
            if(*cntChild == 0 )
                infd = STDIN_FILENO;
            else
                /* other stages read from previous pipe READ_END */
                infd = pipeArr[i-1][READ_END]; 

            /* last stage writes to stdout */
            if(*cntChild == n - 1)
                outfd = STDOUT_FILENO;
            else
                /* other stages write to current pipe WRITE_END */
                outfd = pipeArr[i][WRITE_END];

        /*fprintf(stderr,"%s : in[%d], out[%d]\n",curstage->argv[0], infd, outfd);*/

            /* check and open if infile is present */
            if( curstage->inname!=NULL &&
                (infd = open(curstage->inname, O_RDONLY)) < 0 )
                err_sys("forkChild","open infile");

            /* check and open if outfile is present */
            if( curstage->outname!=NULL &&
                (outfd = open(curstage->outname, 
                            O_WRONLY|O_CREAT|O_TRUNC,PERMS)) < 0 )
                err_sys("forkChild","open outfile");

            /* redirect input */
            if(infd != STDIN_FILENO)
            {
                if(dup2(infd, STDIN_FILENO) < 0)
                    err_sys("forkChild","dup2");
            }
            /* redirect ouput */
            if(outfd != STDOUT_FILENO)
            {
                if(dup2(outfd, STDOUT_FILENO) < 0)
                    err_sys("forkChild","dup2");
            }

            /* close all pipes */
            closePipeArr(pipeArr, n-1);
    
            /* if in/out file are present, close them */
            if(curstage->inname!=NULL)
                close(infd);

            if(curstage->outname!=NULL)
                close(outfd);

            /* unblock SIGINT before launching new process */
            if(sigprocmask(SIG_UNBLOCK, set, NULL) < 0)
                err_sys("forkChild","sigprocmask");

 
            /* execute command of current stage */
            execvp(curstage->argv[0], curstage->argv);

            /* free pipeArr & childArr in child process */
            if(n-1 > 0)
                freePipeArr(&pipeArr, n-1);
            free(childArr);           
 
            /* if execvp returns, print error */
            err_sys("forkChild",curstage->argv[0]);
        }
       
        /* save current child pid in childArr */
        childArr[(*cntChild)] = pid;
 
        (*cntChild)++; 
    }
    
    return 0;     
}

int parentStuff(pipeline pl, int *cntChild, int** pipeArr,
               pid_t* childArr, sigset_t *set)
{
    pid_t pid;
    int i,error = 0, status;


    /* block SIGINT so that waitpid will not be interupted */
    if(sigprocmask(SIG_BLOCK, set, NULL) < 0)
    {
        perror("parentStuff: sigprocmask");
        error = 1;
    }
    
    /* close all pipes */
    closePipeArr(pipeArr, pl->length-1);
 
    /* go through each child */
    for(i=0; i<*cntChild; ++i)    
    {
        pid = childArr[i];
        /* wait for current child */
        if(waitpid(pid,&status, 0) != pid)
        {
            perror("parentStuff: waitpid");
            error = 1;
        }

        /* if current child exited with error, return */
        if((WIFEXITED(status) && WEXITSTATUS(status)) || 
            !(WIFEXITED(status))) 
            error = 1;

    }

    /* unblock SIGINT for next step*/
    if(sigprocmask(SIG_UNBLOCK, set, NULL) < 0)
    {
        perror("parentStuff: sigprocmask");
        error = 1;
    }

 
    return (error==1)?-1:0;
}


int executePipeline(pipeline pl, sigset_t *set)
{
    int len = pl->length, error = 0;
    int** pipeArr = NULL;
    pid_t* childArr = NULL;
    int cntChild = 0;
    

    /*print_pipeline(stdout,pl);
    printf("---\n");
    fflush(stdout);*/

    /* initialize len-1 pipes */
    if(len-1>0)
    {
        if((pipeArr = iniPipeArr(len-1)) == NULL)
            return -1;
    }

    /* array that holds children's pids */
    if((childArr = malloc(len*sizeof(pid_t))) == NULL)
    {
        fprintf(stderr,"executePipeline: ");
        perror("malloc");
        error = 1;
    }
 

    /* if there are no error in initialization step, move on */
    if(!error)
    {
        if(forkChild(pl, &cntChild, pipeArr, childArr, set)<0)
            error = 1;
        if(parentStuff(pl, &cntChild, pipeArr, childArr, set) < 0)
            error = 1;
    }
    
    
    /* if SIGINT was caught, kill any running child processes */
    if(isSigint)
    {
        killChild(childArr, cntChild);
        error = 1;
    }

    /* free allocated memory */
    if(pipeArr)
        freePipeArr(&pipeArr, len-1);

    if(childArr)
        free(childArr);
    
 
    return (error=1) ? -1 : 0;
}

int main(int argc, char* argv[])
{
    FILE* infile;
    char* commandLine = NULL;
    pipeline pl = NULL;
    sigset_t set;
    struct sigaction sa;

    if(argc > 2)
        usage();

    /* register handler for SIGINT */
    sa.sa_flags = 0;
    sa.sa_handler = handler;
    
    if(sigemptyset(&sa.sa_mask) < 0)
        err_sys("main","sigemptyset");

    if(sigaction(SIGINT, &sa, NULL) < 0)
        err_sys("main","sigaction");

    /* add SIGINT to set for sigprocmask later use */
    if(sigemptyset(&set) < 0)
        err_sys("main","sigemptyset");

    if(sigaddset(&set, SIGINT) < 0)
        err_sys("main","sigaddset");
  

    /* infile default is stdin */ 
    infile = stdin;
    /* if mush2 is run with argument, open given file */
    if(argc == 2)
    {
        if((infile = fopen(argv[1], "r")) == NULL)
        {
            perror("fopen");
            exit(EXIT_FAILURE);
        }
    }    

    do
    {
        /* make sure SIGINT is unblocked before processing new command*/
        if(sigprocmask(SIG_UNBLOCK, &set, NULL) < 0)
            err_sys("main","sigprocmask");

        if(infile == stdin)
        {
            printf("%s ",PROMPT);
            fflush(stdout);
        }

        /* read command line */
        if((commandLine = readLongString(infile)) != NULL)
        {
            /*fprintf(stderr,"command: %s\n\n",commandLine);*/

            /* parse command line */
            if((pl=crack_pipeline(commandLine)) !=NULL)
                executePipeline(pl, &set);
        }


        /* if SIGINT was caught, clearerr */
        if(isSigint)
        {
            clearerr(infile);
            isSigint = 0;
        }

        /* free allocated memory */
        if(commandLine)
        {
            free(commandLine);
            commandLine = NULL;
        }

        if(pl)
        {
            free_pipeline(pl);
            pl = NULL;
        }

    /*keep looping until reach EOF or error */   
    }while(!feof(infile) && !ferror(infile));

    if(infile == stdin)
        printf("\n");
   
    return 0;
}