//fib.c
//computes first n fibbonaci

#include <stdlib.h>
#include <stdio.h>

int main(int argc, char* argv[]){
   int i = 0;
   int n = atoi(argv[1]);
   int f1 = 0;
   int f2 = 1;
   int temp;
   
   printf("%d", f1);
   while(i < n){
      printf(" %d", f2);
      temp = f2;
      f2 = f2 + f1;
      f1 = temp;
      i++;
   }
   printf("\n");
   
   return EXIT_SUCCESS;
}
