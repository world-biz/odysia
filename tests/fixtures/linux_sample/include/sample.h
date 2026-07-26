/**
 * sample_state - tracks mutable kernel state
 */
struct sample_state {
    int counter;
    void (*handler)(int value);
};

enum sample_mode {
    SAMPLE_MODE_OFF = 0,
    SAMPLE_MODE_ON = 1
};

typedef struct sample_state sample_state_t;

static int global_count;