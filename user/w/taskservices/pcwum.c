/* The native performance-counter provider registry is not implemented.
 * Never return a query handle or successful data collection without it. */
__declspec(dllexport) long PcwCreateQuery(void **query, unsigned long flags)
{
    (void)flags;
    if (!query) return (long)0xc000000du;
    *query = 0;
    return (long)0xc00000bbu; /* STATUS_NOT_SUPPORTED */
}
__declspec(dllexport) long PcwAddQueryItem(void **item, void *query,
    unsigned char counter, const void *name, const void *instance,
    unsigned long id, unsigned long flags)
{
    (void)query; (void)counter; (void)name; (void)instance; (void)id; (void)flags;
    if (item) *item = 0;
    return (long)0xc00000bbu;
}
__declspec(dllexport) long PcwCollectData(void *query, void *buffer,
    unsigned long bytes, unsigned long *used)
{
    (void)query; (void)buffer; (void)bytes;
    if (used) *used = 0;
    return (long)0xc00000bbu;
}
