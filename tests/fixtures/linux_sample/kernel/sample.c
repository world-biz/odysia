/**
 * helper - simple leaf helper
 */
int helper(int value)
{
    return value + 1;
}

/**
 * sample_log - logs a tagged message
 */
asmlinkage __visible int sample_log(const char *tag, const char *fmt, ...)
{
    int local_value = helper(1);
    struct sample_state state;
    void (*callback)(int value) = state.handler;

    callback(local_value);
    return local_value + (tag != 0) + (fmt != 0);
}