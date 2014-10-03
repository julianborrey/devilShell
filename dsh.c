/*
 * dsh.c
 * by Julian Borrey
 * Last Updated: 21/09/2014
 */

#include "dsh.h"

//length of prompt string including \0
#define PROMPT_BUF_LEN 15

//length of buffer for string to hold path
#define PATH_BUF_LEN 18

//flags for IO files
#define INPUT_FILE_FLAGS      O_RDONLY
#define OUTPUT_FILE_FLAGS    (O_WRONLY | O_TRUNC | O_CREAT)
#define NEW_FILE_PERMISSIONS (S_IRUSR | S_IWUSR)

//path to the black hole to redirect output for a 
//child whoes parent seizes tty late
#define DEV_NULL_PATH "/dev/null"

//code to say we didn't opent the null path
#define NO_BLACKHOLE -1

//when we have no pipe, use an impossible fd for a pipe
#define NO_PIPE -1

//generic error code used in many functions
#define GENERAL_ERROR -1

typedef struct _activeList {
   job_t* job; //the job that is active
   bool crashed; //true is a process in the job crashed
   bool killed;
   struct _activeList* next; //the next node in the LList
} activeJobNode;

//list of active jobs
activeJobNode* activeList;

//string for the prompt
//saves us mallocing and freeing everytime
char promptString[PROMPT_BUF_LEN];

/* given functions */
/* Grab control of the terminal for the calling process pgid.  */
void seize_tty(pid_t callingprocess_pgid); 

void continue_job(job_t *j, bool bg); /* resume a stopped job */

void spawn_job(job_t *j); /* spawn a new job */

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p, bool child);

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
int new_child(job_t *j, process_t *p, int inPipe, int outPipe);

/* builtin_cmd - If the user has typed a built-in command then execute it immediately. */
bool builtin_cmd(job_t *job, int argc, char **argv);

char* promptmsg(pid_t pid); /* Build prompt messaage */


/* my functions */
void cycleThroughEachJob(job_t* firstJob); //does each job

//gets a pointer to a string of the current path
char* getCurrentPath(void);

//updates the IO stream of child process to be for a file
int changeStreamToFile(char* fileName, int stream, int flags);

//makes a job node (malloc's it)
activeJobNode* newJobNode(job_t* j);

//adds job to active lise
activeJobNode* addJobToActiveList(job_t* j);

//removes job from active list
void removeActiveJobFromList(activeJobNode* aj);

//prints the pid, status and cmd of a single job
void printSingleActiveJob(activeJobNode* jn);

//prints the list of active jobs
void printActiveJobs(activeJobNode* list);

//gives back the job number
job_t* getJobToWakeup(char* s);

//finds the job with the given pgid
job_t* findJobByPGID(int pgid);

//clean the active jobs list for processes which 
//crashed and didn't report death
void cleanActiveJobList(activeJobNode* list);

//garbage cleanup for an activeJob struct
void freeActiveJob(activeJobNode* j);

//frees job and all processes
void freeJob(job_t* j);

//updates status fields from value reported from waitpid()
void examineProcesses(job_t* j, activeJobNode* aj);

//check processes are not dead
void checkOnProcesses(activeJobNode* jobNode);

//make all stopped processes in non-stopped state
void unStopStoppedProcesses(job_t* j);


int main(int argc, char* argv[]) {
   init_dsh();
   DEBUG("Successfully initialized\n");
   
   //head of a linked list of all jobs that are active
   activeJobNode* activeList = NULL;

   job_t* j;
   while(1) {
      j = NULL;
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
int new_child(job_t *j, process_t *p, int inPipe, int outPipe)
{  
   p->pid = getpid();
   
   int blackHole = NO_BLACKHOLE;
   if(j->bg){ //if background, and parent hasn't seized tty yet
      //we will redirect to a black hole
      blackHole = open(DEV_NULL_PATH, O_WRONLY);
      if(dup2(blackHole, STDOUT_FILENO) == GENERAL_ERROR){
         perror("Could not redirect stdout no an abyss");
      }
   }
   
   /* also establish child process group in child to avoid race (if parent has not done it yet). */
   set_child_pgid(j, p, true);

   /* DEALING WITH INPUT - WE ALREADY HAVE A PIPE OR USE A FILE */
   //change input stream if < used
   if(inPipe != NO_PIPE) { //use a pipe
      if(dup2(inPipe, STDIN_FILENO) == GENERAL_ERROR){
        perror("Failed to set up input pipe");
      }
   } else if(p->ifile != NULL){
      if(changeStreamToFile(p->ifile, STDIN_FILENO, INPUT_FILE_FLAGS) == GENERAL_ERROR){
        //perror("Error updating input stream");
        return blackHole; //if error, return, don't exec
      }
   } else if(!(j->bg)){
      seize_tty(p->pid);
   }

   /* DEALING WITH OUTPUT - PIPE OR FILE */
   //if this process points to another then we are using a pipe
   if(outPipe != NO_PIPE){
      if(dup2(outPipe, STDOUT_FILENO) == GENERAL_ERROR){
        perror("Failed to set up output pipe");
      }
   } else if(changeStreamToFile(p->ofile, STDOUT_FILENO, OUTPUT_FILE_FLAGS) == GENERAL_ERROR){
      //perror("Error updating output stream");
      return blackHole; //if error, return, don't exec
   }
   
   /* Set the handling for job control signals back to the default. */
   signal(SIGTTOU, SIG_DFL);
   
   //never coming back after this
   execvp(p->argv[0], p->argv);

   return blackHole; //if failed to exec we come here
}

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p, bool child)
{
    if (j->pgid < 0){ /* first child: use its pid for job pgid */
        j->pgid = p->pid;

       if(child && j->bg){
          printf("EXECUTING [%d] (background): %s\n", j->pgid, j->commandinfo);
       } else if(child){
          printf("EXECUTING [%d] (foreground): %s\n", j->pgid, j->commandinfo);
       } else if(child){
       }
    }
        
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
      if(newFd < 0){ //error handling
         return GENERAL_ERROR;
      }

      result = dup2(newFd, stream);   //update stream to map to that file
      close(newFd);                   //close our reference to the file
   }

   return result; //return result fo dup2
}

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
  activeJobNode* aj = addJobToActiveList(j);

  //setup for pipes between the processes
  int fds[2] = {NO_PIPE, NO_PIPE}; //for pipes
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

      case GENERAL_ERROR: /* fork failure */
        perror("fork");
        exit(EXIT_FAILURE);

      case 0: /* child process  */
        p->pid = getpid();
        int blackHole = new_child(j, p, pipeRead, pipeWrite);
        
        //if we return, did they open the black hole?
        if(blackHole != NO_BLACKHOLE){
           close(blackHole);
        }
        
        /* YOUR CODE HERE?  Child-side code for new process. */
        kill(j->pgid, SIGCHLD); //tell parent of failure
        perror("Failed to execute process");
        exit(EXIT_FAILURE);  /* NOT REACHED */
        break;    /* NOT REACHED */

      default: /* parent */
        /* establish child process group */
        p->pid = pid;
        set_child_pgid(j, p, false);
        close(pipeWrite);
        close(pipeRead);
        pipeRead = fds[0];
        
        if(j->bg){             // if background job
          seize_tty(getpid()); // take the terminal
        }

     }
     
     /* YOUR CODE HERE?  Parent-side code for new job.*/
     //seize_tty(getpid()); // assign the terminal back to dsh
   
   }

   //get all the status values of the processes
   examineProcesses(j, aj);
   
   //now we might be finished with the job
   if((!job_is_completed(j)) && job_is_stopped(j)){
      printf("\nJob %d was suspended.\n", j->pgid);
      j->notified = true;
   } else if(aj->crashed){ //if didn't execute
      removeActiveJobFromList(aj);
   }

   //get terminal bach for shell
   seize_tty(getpid());
   return;
}

//fills the status of each processes with status
//reported from waitpid()
void examineProcesses(job_t* j, activeJobNode* aj){
   process_t* current = j->first_process;
   while(current != NULL){
     //get status

     if(j->bg){ //if background job, don't wait on it
        waitpid(current->pid, &(current->status), WNOHANG);
     } else {   //if foreground job we wait on it as the parent
        waitpid(current->pid, &(current->status), WUNTRACED);
     }
     //detemine meaning of status
     if(WSTOPSIG(current->status) == SIGTSTP){ //suspended
        current->stopped = true;
     } else if(WIFEXITED(current->status)){    //if continued
        if(WEXITSTATUS(current->status) == 0){ //if success
           current->completed = true;
        } else { //probably something that couldn't be run
           aj->crashed = true;
        }
     }
     
     //examing next process
     current = current->next;
   }
   return;
}

//makes a job node (malloc's it)
activeJobNode* newJobNode(job_t* j){
   activeJobNode* node = (activeJobNode*) malloc(sizeof(struct _activeList)); 
   node->job = j;
   node->crashed = false;
   node->killed = false;
   node->next = NULL;
   return node;
}

//adds job to active lise
activeJobNode* addJobToActiveList(job_t* j){
   activeJobNode* current = activeList;

   //if first one
   if(current == NULL){ //comparing ptrs
      activeList = newJobNode(j);
      return activeList;
   }

   //not first one, go through list
   while(current->next != NULL){ //iterate through list
      current = current->next;
   }

   current->next = newJobNode(j);
   return (current->next);
}

//updates jobs from active list
void removeActiveJobFromList(activeJobNode* aj){
   activeJobNode* prev = NULL;
   activeJobNode* current = activeList;

   //not first one, go through list
   while(current != NULL){    //iterate through list
      if(current == aj){      //if addresses match
         if(prev == NULL){    //if haven't move from first
           activeList = NULL; //remove the only node on the lsit
         } else {
           prev->next = current->next; //skip over current
         }
         freeActiveJob(current);
         return;                       //to remove from list
      }
      prev = current;
      current = current->next;
   }

   return;
}

//garbage cleanup for an activeJob struct
void freeActiveJob(activeJobNode* aj){
  freeJob(aj->job);
  free(aj);
}

//frees job and all processes
void freeJob(job_t* j){
   process_t* p;
   process_t* pNext;
   if(j != NULL){
      p = j->first_process;
      while(p != NULL){
         pNext = p->next;
         for(int i = 0; i < p->argc; i++){
           free(p->argv[i]);
         }
         free(p->argv);
         free(p->ifile);
         free(p->ofile);
         free(p);
         p = pNext;
      }
   } else {
      return;
   }
}

/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j, bool bg) 
{
   if(kill(j->pgid, SIGCONT) < 0){
      perror("kill(SIGCONT)");
   } else {
      printf("RESUMING [%d]: %s\n", j->pgid, j->commandinfo);
      unStopStoppedProcesses(j);
      int status;
      if(bg){
        waitpid(j->pgid, &status, WNOHANG);
      } else {
        waitpid(j->pgid, &status, WUNTRACED);
      }
      /////////////////////////makeAllComplete(j);
      j->bg = bg;
      if(bg){ //if bg, get the terminal
         seize_tty(getpid());
      }
   }
   return;
}

//make all stopped processes in non-stopped state
void unStopStoppedProcesses(job_t* j){
   process_t* current = j->first_process;
   while(current != NULL){
      if(current->stopped){
         current->stopped = false;
      }
      current = current->next;
   }
   return;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
bool builtin_cmd(job_t* job, int argc, char **argv){

   /* check whether the cmd is a built in command */
   
   if (!strcmp(argv[0], "quit")) {
   
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
   
     //choose job
     job_t* job = getJobToWakeup(argv[1]);

     //start the job's stopped process
     if(job != NULL){
        continue_job(job, true); //continue job in bg
     } else {
        printf("Could not find job to continue.\n");
     }
     return true;
   
   } else if (!strcmp("fg", argv[0])) {
   
     //choose job
     job_t* job = getJobToWakeup(argv[1]);

     //start the job's stopped process
     if(job != NULL){
        continue_job(job, false); //continue job in fg
     } else {
        printf("Could not find job to continue.\n");
     }
     return true;
   
   }

   return false; /* not a builtin command */
}

//gives back the job number
job_t* getJobToWakeup(char* s){
   if(s != NULL){
      return findJobByPGID(atoi(s));
   } else {
      return find_last_job(activeList); //other case to handle
   }
   return NULL;
}

//finds the job with the given pgid
job_t* findJobByPGID(int pgid){
   activeJobNode* current = activeList;
   while(current != NULL){
      if((current->job->pgid) == pgid){
         return current->job;
      }
      current = current->next;
   }
   return NULL;
}

//prints the list of active jobs
void printActiveJobs(activeJobNode* list){
   //clean list
   //important for case where a process crashes and doesn't send signal
   cleanActiveJobList(list);

   if(list != NULL){
     printf("Active jobs:\n");

     activeJobNode* current = list;
     while(current != NULL){
        printSingleActiveJob(current);
        current = current->next;
     }
   } else {
     printf("No active jobs.\n");
   }
   
   return;
}

//cleans the active job list for programs not running
//important for programs which crashed/finished and didn't signal back to parent
void cleanActiveJobList(activeJobNode* list){
   activeJobNode* current = list;
   while(current != NULL){ //iterate through list
     checkOnProcesses(current);
     current = current->next;
   }
   return;
}

//check processes are not dead
void checkOnProcesses(activeJobNode* jobNode){
   process_t* current = jobNode->job->first_process;
   int result;

   while(current != NULL){
      result = waitpid(current->pid, &(current->status), WNOHANG);

      if(result != 0){ //dead process
         if(WEXITSTATUS(current->status) != 0){ //if not exit code 0 (success)
            jobNode->crashed = true;    //not this for the printing
         } else if(WIFSIGNALED(current->status)){
            jobNode->killed = true;
         } else {
            current->completed = true;
         }
      }

      current = current->next;
   }
   return;
}

//prints the pid, status and cmd of a single job
void printSingleActiveJob(activeJobNode* jn){
   char foregroundStr[] = "foreground";
   char backgroundStr[] = "background";
   
   char* groundStr = foregroundStr;
   if(jn->job->bg){ 
      groundStr = backgroundStr;
   }

   if(jn->crashed){
      printf("\t[%d] (%s) ~ %s ~ %s\n", jn->job->pgid, groundStr, " CRASHED ", jn->job->commandinfo);
      removeActiveJobFromList(jn);
   } else if(jn->killed){
      printf("\t[%d] (%s) ~ %s ~ %s\n", jn->job->pgid, groundStr, "SIGNAL TERMINATED", jn->job->commandinfo);
      removeActiveJobFromList(jn);
   } else if(job_is_completed(jn->job)){
      printf("\t[%d] (%s) ~ %s ~ %s\n", jn->job->pgid, groundStr, "COMPLETED", jn->job->commandinfo);
      removeActiveJobFromList(jn);  
   } else if(job_is_stopped(jn->job)) {
      //compeleted job, remove from list
      printf("\t[%d] (%s) ~ %s ~ %s\n", jn->job->pgid, groundStr, "SUSPENDED", jn->job->commandinfo);
   } else {
      printf("\t[%d] (%s) ~ %s ~ %s\n", jn->job->pgid, groundStr, " ACTIVE  ", jn->job->commandinfo);
   }
   return;
}

/* Build prompt messaage */
char* promptmsg(pid_t pid){
  if(isatty(STDIN_FILENO)){ //if we have input from terminal
    sprintf(promptString, "dsh[%d]$ ", (int) pid); //print prompt
  } else {
    sprintf(promptString, ""); //other wise we print blank (nothing)
  }
	return promptString;
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