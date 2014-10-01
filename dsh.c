/*
 * dsh.c
 * by Julian Borrey
 * Last Updated: 21/09/2014
 */

#include "dsh.h"

//length of prompt string including \0
#define PROMPT_BUF_LEN 15

//length of buffer for string to hold path
#define PATH_BUF_LEN 1024

//flags for IO files
#define INPUT_FILE_FLAGS     O_RDONLY
#define OUTPUT_FILE_FLAGS    (O_WRONLY | O_TRUNC | O_CREAT)
#define NEW_FILE_PERMISSIONS (S_IRUSR | S_IWUSR)

//when we have no pipe, use an impossible fd for a pipe
#define NO_PIPE -1

typedef struct _activeList {
   job_t* job; //the job that is active
   struct _activeList* next; //the next node in the LList
} activeJobNode;

//list of active jobs
activeJobNode* activeList;

//given functions
 /* Grab control of the terminal for the calling process pgid.  */
void seize_tty(pid_t callingprocess_pgid); 

void continue_job(job_t *j); /* resume a stopped job */

void spawn_job(job_t *j); /* spawn a new job */

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p);

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p, int inPipe, int outPipe);

/* builtin_cmd - If the user has typed a built-in command then execute it immediately. */
bool builtin_cmd(job_t *job, int argc, char **argv);

char* promptmsg(); /* Build prompt messaage */


//my functions
void cycleThroughEachJob(job_t* firstJob); //does each job

//gets a pointer to a string of the current path
char* getCurrentPath(void);

//updates the IO stream of child process to be for a file
int changeStreamToFile(char* fileName, int stream, int flags);

//makes a job node (malloc's it)
activeJobNode* newJobNode(job_t* j);

//adds job to active lise
void addJobToActiveList(job_t* j);

//removes job from active list
void removeJobFromActiveList(job_t* j);

//prints the pid, status and cmd of a single job
void printSingleActiveJob(job_t* j);

//prints the list of active jobs
void printActiveJobs(activeJobNode* list);


int main(int argc, char* argv[]) {
   init_dsh();
   DEBUG("Successfully initialized\n");
   
   //head of a linked list of all jobs that are active
   activeJobNode* activeList = NULL;

   while(1) {
      job_t *j = NULL;
      if(!(j = readcmdline(promptmsg(getpid())))) {
         if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            printf("\n");
            exit(EXIT_SUCCESS);
         }
         continue; /* NOOP; user entered return or spaces with return */
      }
      
      //do each job
      cycleThroughEachJob(j);
        
      /* Only for debugging purposes to show parser output; turn off in the
       * final code */
      //if(PRINT_INFO) print_job(j);
      

      /* Your code goes here */
      /* You need to loop through jobs list since a command line can contain ;*/
      /* Check for built-in commands */
      /* If not built-in */
          /* If job j runs in foreground */
          /* spawn_job(j,true) */
          /* else */
          /* spawn_job(j,false) */
    }
}

//does each job
void cycleThroughEachJob(job_t* firstJob){
  //now we have a list of jobs starting at j*
  //for each, must check if we have a built in command or process
  job_t* currentJob = firstJob;
  
  /////////////////// currently only supports builtin in as argv[0]
  
  while(currentJob != NULL){ //while not at end of list
     if(!builtin_cmd(currentJob, 
                     currentJob->first_process->argc,
                     currentJob->first_process->argv)){ //for process
        spawn_job(currentJob);
     }
     currentJob = currentJob->next; //check out next job
  }

  return;
}

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p, int inPipe, int outPipe)
{  
   /* establish a new process group, and put the child in
   * foreground if requested
   */
   
   /* Put the process into the process group and give the process
   * group the terminal, if appropriate.  This has to be done both by
   * the dsh and in the individual child processes because of
   * potential race conditions.  
   * */
   p->pid = getpid();
   
   /* also establish child process group in child to avoid race (if parent has not done it yet). */
   set_child_pgid(j, p);

   /* DEALING WITH INPUT - WE ALREADY HAVE A PIPE OR USE A FILE */
   //change input stream if < used
   if(inPipe != NO_PIPE) { //use a pipe
      dup2(inPipe, STDIN_FILENO);
   } else if(changeStreamToFile(p->ifile, STDIN_FILENO, INPUT_FILE_FLAGS) == -1){
      //perror("Error updating input stream");
      return; //if error, return, don't exec
   } else {

   }

   /* DEALING WITH OUTPUT - PIPE OR FILE */
   //if this process points to another then we are using a pipe
   if(outPipe != NO_PIPE){
      dup2(outPipe, STDOUT_FILENO);
   } else if(changeStreamToFile(p->ofile, STDOUT_FILENO, OUTPUT_FILE_FLAGS) == -1){
      //perror("Error updating output stream");
      return; //if error, return, don't exec
   }
   
   /* Set the handling for job control signals back to the default. */
   signal(SIGTTOU, SIG_DFL);
   //never coming back after this
   execvp(p->argv[0], p->argv);

   return; //token return
}

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p)
{
    if (j->pgid < 0) /* first child: use its pid for job pgid */
        j->pgid = p->pid;
    return(setpgid(p->pid,j->pgid)); //set pgid of process to put it in the group
}

//updates the IO stream of child process to be for a file
int changeStreamToFile(char* fileName, int stream, int flags){
   int newFd;         //for the file we open
   int result = 0;    //for the result of dup2
   //0 by default because if fileName == NULL, 
   //there was no input file andwe do nothing

   if(fileName != NULL){              //if we have a file we continue
      newFd  = open(fileName, flags, NEW_FILE_PERMISSIONS); //get fd for file
      result = dup2(newFd, stream);   //update stream to map to that file
      close(newFd);                   //close our reference to the file
   }

   return result; //return result fo dup2
}


////////// CHEKC IF ABOVE WILL CHANGE PERMISSIONS EVEN ON A READ




/* Spawning a process with job control. fg is true if the 
 * newly-created process is to be placed in the foreground. 
 * (This implicitly puts the calling process in the background, 
 * so watch out for tty I/O after doing this.) pgid is -1 to 
 * create a new job, in which case the returned pid is also the 
 * pgid of the new job.  Else pgid specifies an existing job's 
 * pgid: this feature is used to start the second or 
 * subsequent processes in a pipeline.
 * */

void spawn_job(job_t *j) 
{
  
	pid_t pid;
	process_t *p;
  
  //register this job as active
  addJobToActiveList(j);

  //setup for pipes between the processes
  int fds[2] = {NO_PIPE, NO_PIPE}; //for pipes
  int status;
  int pipeWrite = NO_PIPE;
  int pipeRead = NO_PIPE;

	for(p = j->first_process; p; p = p->next) {
    /* YOUR CODE HERE? */
	  /* Builtin commands are already taken care earlier */
    
    close(pipeWrite);
	  if(p->next != NULL){ //if there is a pipe here
       pipe(fds); //get a pipe
       pipeWrite = fds[1];
    } else {
       pipeWrite = NO_PIPE;
    }

	  switch (pid = fork()) {

      case -1: /* fork failure */
        perror("fork");
        exit(EXIT_FAILURE);

      case 0: /* child process  */
        p->pid = getpid();
        new_child(j, p, pipeRead, pipeWrite);
        
        /* YOUR CODE HERE?  Child-side code for new process. */
        perror("Failed to execute process");
        exit(EXIT_FAILURE);  /* NOT REACHED */
        break;    /* NOT REACHED */

      default: /* parent */
        /* establish child process group */
        p->pid = pid;
        set_child_pgid(j, p);
        close(pipeRead);
        pipeRead = fds[0];
        
        if(!(j->bg)){         // if not background is set
          seize_tty(j->pgid); // assign the terminal
        }

     }
     
     /* YOUR CODE HERE?  Parent-side code for new job.*/
     //seize_tty(getpid()); // assign the terminal back to dsh
   
   }

   //we have run all children concurrently
   //now we wait for them all to finish
   waitpid((-1 * j->pgid), &status, 0);
   //ensures that jobs are run sequentially

   //now finished with the job
   removeJobFromActiveList(j);

   //get terminal bach for shell
   seize_tty(getpid());

   return;
}

//makes a job node (malloc's it)
activeJobNode* newJobNode(job_t* j){
   activeJobNode* node = (activeJobNode*) malloc(sizeof(struct _activeList)); 
   node->job = j;
   node->next = NULL;
   return node;
}

//adds job to active lise
void addJobToActiveList(job_t* j){
   activeJobNode* current = activeList;

   //if first one
   if(current == NULL){ //comparing ptrs
      activeList = newJobNode(j);
      return;
   }

   //not first one, go through list
   while(current->next != NULL){ //iterate through list
      current = current->next;
   }

   current->next = newJobNode(j);
   return;
}

//removes job from active list
void removeJobFromActiveList(job_t* j){
   activeJobNode* prev = NULL;
   activeJobNode* current = activeList;

   //if first one
   if((current->job) == j){ //comparing ptrs
      activeList = activeList->next; //skip over
      free(current);
      return;
   }

   //not first one, go through list
   while(current != NULL){ //iterate through list
      if((current->job) == j){ //if addresses match
         prev->next = current->next; //skip over current
         free(current);
         return;                     //to remove from list
      }
      prev = current;
      current = current->next;
   }

   return;
}

/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j) 
{
   if(kill(j->pgid, SIGCONT) < 0)
    perror("kill(SIGCONT)");
}


/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
bool builtin_cmd(job_t* job, int argc, char **argv){

   /* check whether the cmd is a built in command */
   
   if (!strcmp(argv[0], "quit")) {
   
      /* Your code here */ //??????????????????????
      exit(EXIT_SUCCESS);
   
   } else if (!strcmp("jobs", argv[0])) {
      
      //our list is already in sorted order
      //we just have to print the list
      printActiveJobs(activeList);
      
      return true;
   
   } else if (!strcmp("cd", argv[0])) {
   
      //change directory
      char* oldPath = getCurrentPath();

      if(chdir(argv[1]) < 0){ //returns -1 if error
         perror("Problem changing directory");
      } else {
         char* newPath = getCurrentPath();
         printf("%s\n%s\n", oldPath, newPath);
      }
      return true;
   
   } else if (!strcmp("bg", argv[0])) {
   
     /* Your code here */
     return true;
   
   } else if (!strcmp("fg", argv[0])) {
   
     /* Your code here */
     return true;
   
   }
   return false;       /* not a builtin command */
}

//prints the list of active jobs
void printActiveJobs(activeJobNode* list){
   printf("Active jobs:\n");

   activeJobNode* current = list;
   while(current != NULL){
      printSingleActiveJob(current->job);
      current = current->next;
   }
   
   return;
}

//prints the pid, status and cmd of a single job
void printSingleActiveJob(job_t* j){
   
      printf("\t[%d] - %s - %s", j->pgid, "Active", j->commandinfo);
    
}

/* Build prompt messaage */
char* promptmsg(pid_t pid){
  /* Modify this to include pid */
  char* str = (char*) malloc(PROMPT_BUF_LEN * sizeof(char));
  sprintf(str, "dsh[%d]$ ", (int) pid);
	return str;
}

//gets a pointer to a string of the current path
char* getCurrentPath(void){
   char* buf = (char*) malloc(PATH_BUF_LEN * sizeof(char));
   if(getcwd(buf, PATH_BUF_LEN) == (char*)-1) { //if failure
      perror("Cannot get path");
      buf = NULL;
   }
   return buf;
}

