uint64_t now_msec() {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000 + ( now.tv_usec / 1000 );
}
