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

//the numbers for stream
#define STANDARD_INPUT 0
#define STANDARD_OUTPUT 1
#define STANDARD_ERROR 2

 //flags for IO files
 #define INPUT_FILE_FLAGS     O_RDONLY
 #define OUTPUT_FILE_FLAGS    (O_WRONLY | O_TRUNC | O_CREAT)
 #define NEW_FILE_PERMISSIONS (S_IRUSR | S_IWUSR)

//given functions
 /* Grab control of the terminal for the calling process pgid.  */
void seize_tty(pid_t callingprocess_pgid); 

void continue_job(job_t *j); /* resume a stopped job */

void spawn_job(job_t *j); /* spawn a new job */

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p);

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p);

/* builtin_cmd - If the user has typed a built-in command then execute it immediately. */
bool builtin_cmd(job_t *job, int argc, char **argv);

char* promptmsg(); /* Build prompt messaage */


//my functions
void cycleThroughEachJob(job_t* firstJob); //does each job

//gets a pointer to a string of the current path
char* getCurrentPath(void);

//updates the IO stream of child process to be for a file
int changeStreamToFile(char* fileName, int stream, int flags);


int main(int argc, char* argv[]) {
   init_dsh();
   DEBUG("Successfully initialized\n");
   
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
void new_child(job_t *j, process_t *p)
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
   
   //change input stream if < used
   if(changeStreamToFile(p->ifile, STDIN_FILENO, INPUT_FILE_FLAGS) == -1){
      //perror("Error updating input stream");
      return; //if error, return, don't exec
   }

   //change output stream if > used
   if(changeStreamToFile(p->ofile, STDOUT_FILENO, OUTPUT_FILE_FLAGS) == -1){
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

	for(p = j->first_process; p; p = p->next) {

	  /* YOUR CODE HERE? */
	  /* Builtin commands are already taken care earlier */
	  
	  switch (pid = fork()) {

          case -1: /* fork failure */
            perror("fork");
            exit(EXIT_FAILURE);

          case 0: /* child process  */
            p->pid = getpid();
            new_child(j, p);
            
	          /* YOUR CODE HERE?  Child-side code for new process. */
            perror("Failed to execute process");
            exit(EXIT_FAILURE);  /* NOT REACHED */
            break;    /* NOT REACHED */

          default: /* parent */
            /* establish child process group */
            p->pid = pid;
            set_child_pgid(j, p);
            int status;

            if(!(j->bg))          // if not background is set
              seize_tty(j->pgid); // assign the terminal

            waitpid(p->pid, &status, 0);

            /* YOUR CODE HERE?  Parent-side code for new process.  */
     }

            /* YOUR CODE HERE?  Parent-side code for new job.*/
     seize_tty(getpid()); // assign the terminal back to dsh
   
   }
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
      //print_job(activeJobs);
      
      return true;
   
   } else if (!strcmp("cd", argv[0])) {
   
      //change directory
      char* oldPath = getCurrentPath();

      if(chdir(argv[1]) < 0){ //returns -1 if error
         perror("Problem changing directory");
      } else {
         char* newPath = getCurrentPath();

         /*
         int numCharDiff = getDifference(oldPath, newPath);
         if(numCharDiff > 0){        //new path was bigger
            newPath = highlightPathDiff(newPath, numCharDiff);
         } else if(numCharDiff < 0){ //old path was bigger
            oldPath = highlightPathDiff(oldPath, numCharDiff);
         } //otherwise we had numCharDiff == 0 and so we do nothing
         */

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

