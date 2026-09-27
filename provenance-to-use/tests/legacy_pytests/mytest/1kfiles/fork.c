#include <unistd.h>
#include <stdio.h>

int main() {
    int i;
    for (i=0; i<10; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // child
            printf("child start\n");
            FILE *f = fopen("test.txt", "r");
            fclose(f);
            printf("child exit\n");
            return 0;
        }
    
        // parent: do nothing, make another child in the next loop
    }
    return 0;
}

