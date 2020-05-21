#define BIG 10000
int big[BIG][BIG];

int main(void) {
    for (int i = 0; i < BIG; i++) {
        for (int j = 0; j < BIG; j++) {
            big[j][i] = 0;
        }
    }
    return 0;
}

