#include <stdio.h>
#include <unistd.h>

#define PAUSE do {usleep(1000000);} while (0)

int main() {
    FILE *in, *out;
    
    PAUSE;
    in = fopen("in1.txt", "r");
    PAUSE;
    out = fopen("out2.txt", "w");
    PAUSE;
    fclose(in);
    PAUSE;
    fclose(out);
    PAUSE;
    in = fopen("in3.txt", "r");
    PAUSE;
    fclose(in);
    PAUSE;
    out = fopen("out4.txt", "w");
    PAUSE;
    fclose(out);
    
    return 0;
}

