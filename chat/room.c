#include <stdlib.h>

#include "room.h"

void shuffle(int *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = random() % (i + 1);
        int tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}
