#include "build.h"

/**
 * global variables
 * */
config_t      gconfig;

hash_list_ext_t *hash_table  = NULL;
pthread_mutex_t *hash_table_mutex = NULL;
/** 为了提高性能,将原始文件前繈与权重加载到内存 */
orig_list_t     *orig_list   = NULL;
task_queue_t    *task_queue_head = NULL;
task_queue_t    *task_queue_tail = NULL;
pthread_mutex_t  task_queue_mutex;
output_fp_t     *output_fp = NULL;

char g_original_file[FILE_NAME_LEN] = DEFAULT_ORIGINAL_FILE;
int  g_parse_completed = 0;
int  g_thread_num      = THREAD_NUM;
/** END for global vars */

static void init_config()
{
    gconfig.log_level    = 4;
    gconfig.max_hash_table_size = MAX_HASH_TABLE_SIZE;
    snprintf(gconfig.hostname, HOST_NAME_LEN, "%s", DEFAULT_LISTEN);
    snprintf(gconfig.inverted_index, FILE_NAME_LEN, "%s", DEFAULT_INVERTED_INDEX);
    snprintf(gconfig.index_dict, FILE_NAME_LEN, "%s", DEFAULT_INDEX_DICT);

    return;
}

static int init_hash_table()
{
    int ret = 0;
    int i   = 0;
    size_t size = 0;

    /** 申请倒排表的内存空间 */
    size = sizeof(hash_list_ext_t) * gconfig.max_hash_table_size;
    hash_table = (hash_list_ext_t *)malloc(size);
    if (NULL == hash_table)
    {
        fprintf(stderr, "Can NOT malloc memory for hash table, need size: %ld\n",
            size);
        ret = -1;
        goto FINISH;
    }

    for (i = 0; i < gconfig.max_hash_table_size; i++)
    {
        hash_table[i].next = NULL;

        size = sizeof(char) * QUERY_LEN;
        hash_table[i].prefix = (char *)malloc(size);
        if (NULL == hash_table[i].prefix)
        {
            fprintf(stderr, "Can NOT malloc memory for ndex_hash_table[%d].prefix, need size: %ld\n",
                i, size);
            ret = -1;
            goto FINISH;
        }
        hash_table[i].prefix[0] = '\0';
    }

    /** 为 hash 表的每条拉链申请互斥锁,防止写乱 */
    size = sizeof(pthread_mutex_t) * gconfig.max_hash_table_size;
    hash_table_mutex = (pthread_mutex_t *)malloc(size);
    if (NULL == hash_table_mutex)
    {
        fprintf(stderr, "Can NOT malloc memory for hash table mutex, need size: %ld\n",
            size);
        ret = -1;
        goto FINISH;
    }
    /** 初始化 hash 表互斥锁 */
    for (i = 0; i < gconfig.max_hash_table_size; i++)
    {
        pthread_mutex_init(&(hash_table_mutex[i]), NULL);
    }

FINISH:
    return ret;
}

static void usage()
{
    printf(BUILD_PACKAGE " " BUILD_VERSION "\n");
    printf("-s <num>      max hash table size(default: %d)\n", MAX_HASH_TABLE_SIZE);
    printf("-T <num>      set worker thread number(default: %d)\n", THREAD_NUM);
    printf("-v            show version and help\n"
           "-l <level>    set log lever\n"
           "-o <file>     set original file name adn path, ABS path is better\n"
           "-i <file>     set inverted index file name and path, ABS path is better\n"
           "-x <file>     set index dict file name and path, ABS path is better\n"
           "-h            show this help and exit\n");

    return;
}

void *parse_task(void *arg)
{
    char line_buf[ORIGINAL_LINE_LEN];
    char prefix[PREFIX_LEN];
    char *p = NULL;

    FILE *fp = NULL;
    int   i  = 0;
    int   k  = 0;
    int result = 0;
    int chinese_char_flag = 0;
    size_t size = 0;

    indext_t dict_id   = 0;
    task_queue_t *task = NULL;

    fp = fopen(g_original_file, "r");
    if (! fp)
    {
        fprintf(stderr, "Can open original file: [%s]\n", g_original_file);

        goto FINISH;
    }

    while (! feof(fp))
    {
        if ((NULL == fgets(line_buf, ORIGINAL_LINE_LEN - 1, fp)) ||
                '#' == line_buf[0])
        {
            continue;
        }

        line_buf[ORIGINAL_LINE_LEN - 1] = '\0';
        // logprintf("every line: [%s]", line_buf);
        dict_id++;
        chinese_char_flag = 0;

        /** 创建工作队列单个任务 */
        size = sizeof(task_queue_t);
        task = (task_queue_t *)malloc(size);
        if (NULL == task)
        {
            fprintf(stderr, "Can NOT malloc memory for task, task_id = %lu, need size: %lu\n",
                dict_id, size);
            g_parse_completed = 1;
            goto FINISH;
        }

        task->next = NULL;
        task->dict_id = dict_id;
        snprintf(task->original_line, ORIGINAL_LINE_LEN, "%s", line_buf);

        if (NULL != (p = strstr(line_buf, SEPARATOR)))
        {
            *p = '\0';
        }
        //logprintf("every line: [%s]", line_buf);
        k = 0;
        task->prefix_array.count = 0;
        for (i = 1; i <= MB_LENGTH && k < PREFIX_ARRAY_SIZE; i++)
        {
            /** Cleared buffer */
            memset(prefix, 0, sizeof(prefix));

            strtolower(line_buf, strlen(line_buf), DEFAULT_ENCODING);
            result = cut_str(line_buf, prefix, sizeof(prefix), DEFAULT_ENCODING, i, "");
            if (! chinese_char_flag && result != i)
            {
                chinese_char_flag = 1;
            }

            k = task->prefix_array.count;
            snprintf(task->prefix_array.data[k], PREFIX_LEN, "%s", prefix);
            task->prefix_array.count++;
#if (_DEBUG)
            logprintf("every prefix: [%s]", prefix);
#endif

            if (result >= strlen(line_buf))
            {
                break;
            }
        }

        /** 全拼和简拼 */
        // TODO
        if (chinese_char_flag)
        {
            ;
        }

        pthread_mutex_lock(&task_queue_mutex);
        if (NULL == task_queue_head && NULL == task_queue_tail)
        {
            task_queue_head = task;
            task_queue_tail = task;
        }
        else if (NULL == task_queue_head && NULL != task_queue_tail)
        {
            task_queue_head = task_queue_tail;
        }
        else if (task_queue_tail)
        {
            task_queue_tail->next = task;
            task_queue_tail = task;
        }
        else
        {
            fprintf(stderr, "task_queue has something wrong.\n");
            pthread_mutex_unlock(&task_queue_mutex);
            goto FINISH;
        }
        pthread_mutex_unlock(&task_queue_mutex);
    }

    fprintf(stderr, "parse process completed.\n");
    fclose(fp);

FINISH:
    g_parse_completed = 1;
    return NULL;
}

static int load_original()
{
    int ret = 0;
    char line_buf[ORIGINAL_LINE_LEN];
    char *p = NULL;
    FILE *orig_fp     = NULL;
    size_t size = 0;

    orig_list_t *tmp_orig_list = NULL;
    orig_list_t *orig_list_curr= NULL;

    orig_fp = fopen(g_original_file, "r");
    if (! orig_fp)
    {
        fprintf(stderr, "Can open original file: [%s]\n", g_original_file);
        ret = 11;
        goto FINISH;
    }

    while (! feof(orig_fp))
    {
        memset(line_buf, 0, ORIGINAL_LINE_LEN);
        if ((NULL == fgets(line_buf, ORIGINAL_LINE_LEN - 1, orig_fp)) ||
                '#' == line_buf[0])
        {
            continue;
        }

        /** 为了节省内存,只导入 query 热度(权重)*/
        line_buf[ORIGINAL_LINE_LEN - 1] = '\0';
        if (NULL != (p = strstr(line_buf, SEPARATOR)))
        {
            /** original query */
            p++;
        }
        /** trim brief info */
        if (p && NULL != (p = strstr(p, SEPARATOR)))
        {
            *p = '\0';
        }

        size = sizeof(orig_list_t);
        tmp_orig_list = (orig_list_t *)malloc(size);
        if (NULL == tmp_orig_list)
        {
            fprintf(stderr, "Can NOT malloc memory for tmp_orig_list, need size: %ld\n", size);
            ret = -1;
            goto FINISH;
        }

        size = strlen(line_buf) + 1;
        tmp_orig_list->orig_line = (char *)malloc(size);
        if (NULL == tmp_orig_list->orig_line)
        {
            fprintf(stderr, "Can NOT malloc memory for tmp_orig_list->orig_line, need size: %ld\n", size);
            ret = -1;
            goto FINISH;
        }

        tmp_orig_list->next = NULL;
        snprintf(tmp_orig_list->orig_line, size, "%s", line_buf);

        if (NULL == orig_list)
        {
            orig_list      = tmp_orig_list;
            orig_list_curr = tmp_orig_list;
        }
        else
        {
            orig_list_curr->next = tmp_orig_list;
            orig_list_curr = tmp_orig_list;
        }
    }

    fclose(orig_fp);

FINISH:
    return ret;
}

static int weight_cmp(const void *a, const void *b)
{
    weight_item_t *aa = (weight_item_t *)a;
    weight_item_t *bb = (weight_item_t *)b;

    return bb->weight - aa->weight;
}

void *handle_task(void *arg)
{
    argument_t *thread_arg = (argument_t *)arg;
    const int tindex = thread_arg->tindex;

    int i = 0;
    int k = 0;
    int hash_exist = 0;

    FILE *inverted_fp = output_fp[tindex].inverted_fp;   // inverted table
    FILE *dict_fp     = output_fp[tindex].dict_fp;

    indext_t     hash_key = 0;
    indext_t     dict_id  = 0;
    hash_list_ext_t *hash_item     = NULL;
    hash_list_ext_t *tmp_hash_item = NULL;
    orig_list_t     *tmp_orig_list = NULL;
    weight_array_t   weight_array;
    task_queue_t    *task = NULL;

    char tmp_buf[ORIGINAL_LINE_LEN];
    char line_buf[ORIGINAL_LINE_LEN];
    char log_buf[LOG_BUF_LEN];
    char *p = NULL;
    char *find = NULL;
    size_t size = 0;

    size_t prefix_len = 0;
    size_t str_len    = 0;
    int prefix_match  = 0;
    int count = 0;
    float weight = 0.0;

    while (1)
    {
        if (g_parse_completed && NULL == task_queue_head)
        {
            fprintf(stderr, "  [%d]My job has done.\n", tindex);
            break;
        }
#if (_DEBUG)
        fprintf(stderr, "I am worker: %d\n", tindex);
#endif

        pthread_mutex_lock(&task_queue_mutex);
        if (NULL == task_queue_head)
        {
            usleep((useconds_t)(1));
            pthread_mutex_unlock(&task_queue_mutex);
            continue;
        }

        task = task_queue_head;
        task_queue_head = task_queue_head->next;
        pthread_mutex_unlock(&task_queue_mutex);

#if (_DEBUG)
        logprintf("get handle task id: [%lu]", task->dict_id);
        logprintf("recv data: [%s]", task->original_line);
#endif

        /** handle every prefix */
        for (i = 0; i < task->prefix_array.count; i++)
        {
            hash_key = hash(task->prefix_array.data[i], gconfig.max_hash_table_size);
            if (hash_key < 0)
            {
                fprintf(stderr, "hash('%s') error.\n", task->prefix_array.data[i]);
                break;
            }

            /** 前缀 hash 去重 { */
            hash_exist = 0;
            hash_item = &(hash_table[hash_key]);
            while (hash_item && '\0' != hash_item->prefix[0])
            {
#if (_DEBUG)
                logprintf("hash_item->prefix:[%s] CMP task.prefix_array.data[%d] : [%s]",
                    hash_item->prefix, i, task->prefix_array.data[i]);
#endif
                if ((u_char)hash_item->prefix[0] == (u_char)task->prefix_array.data[i][0])
                {
                    hash_exist = 1;
#if (_DEBUG)
                    logprintf("find exist prefix: [%s]", task->prefix_array.data[i]);
#endif
                    break;
                }
                hash_item = hash_item->next;
            }
            /** 如果 hash_key 存在,说明已经处理过了,本条跳过 */
            if (hash_exist)
            {
                //logprintf("multiple: [%s]", task.prefix_array.data[i]);
                continue;
            }
            /** 写 hash 表需要加锁 */
            pthread_mutex_lock(&(hash_table_mutex[tindex]));
            if (NULL == hash_item)
            {
                /** 如果申请内存失败,则释放锁,防止死锁 */
                //logprintf("---- at create new memory ---");
                hash_item = &(hash_table[hash_key]);
                while (hash_item->next)
                {
                    hash_item = hash_item->next;
                }

                size = sizeof(hash_list_ext_t);
                tmp_hash_item = (hash_list_ext_t *)malloc(size);
                if (NULL == tmp_hash_item)
                {
                    fprintf(stderr, "Can NOT malloc memory for tmp_hash_item, need size: %ld\n",
                        size);
                    pthread_mutex_unlock(&(hash_table_mutex[tindex]));
                    goto FATAL_ERROR;
                }

                size = sizeof(char) * QUERY_LEN;
                tmp_hash_item->prefix = (char *)malloc(size);
                if (NULL == tmp_hash_item->prefix)
                {
                    fprintf(stderr, "Can NOT malloc memory for tmp_hash_item->prefix, need size: %ld\n",
                        size);
                    pthread_mutex_unlock(&(hash_table_mutex[tindex]));
                    goto FATAL_ERROR;
                }

                tmp_hash_item->next = NULL;
                hash_item->next = tmp_hash_item;
                hash_item = tmp_hash_item;
            }
            /** 记录已经处理过的到 hash 表中 */
            snprintf(hash_item->prefix, QUERY_LEN, "%s", task->prefix_array.data[i]);
#if (_DEBUG)
            logprintf("[Recond]hash_item->prefix = %s", task->prefix_array.data[i]);
#endif
            pthread_mutex_unlock(&(hash_table_mutex[tindex]));
            /** end of 去重 }*/

            dict_id = 0;
            memset(&weight_array, 0, sizeof(weight_array_t));

            /** 扫描整个输入词表,建立索引 */
            tmp_orig_list = orig_list;
            while (tmp_orig_list)
            {
#if (_DEBUG)
                logprintf("tmp_orig_list->orig_line: %s", tmp_orig_list->orig_line);
#endif
                snprintf(line_buf, ORIGINAL_LINE_LEN, "%s", tmp_orig_list->orig_line);
                /** tmp_buf 可以安全复用 */
                snprintf(tmp_buf, sizeof(tmp_buf), "%s", line_buf);
                find = tmp_buf;
                if (NULL != (p = strstr(tmp_buf, SEPARATOR)))
                {
                    *p = '\0';
                    find = p + 1;
                }
                strtolower(tmp_buf, strlen(tmp_buf), DEFAULT_ENCODING);

                dict_id++;
                p = task->prefix_array.data[i];
                prefix_len   = strlen(p);
                str_len      = strlen(tmp_buf);
                prefix_match = 0;
                for (k = 0; k < prefix_len && k < str_len; k++)
                {
                    if(p[k] == tmp_buf[k])
                        prefix_match++;
                    else
                        break;
                }

                if (! prefix_match || prefix_match != prefix_len)
                {
                    goto LOOP_NEXT;
                }

                p = find;
                // logprintf("weight str: %s", p);
                weight = 0;
                if (p)
                    weight = (float)atof(p);

                count = weight_array.count;
                if(count < DEFAULT_WEIGHT_ARRAY_SIZE)
                {
                    weight_array.weight_item[count].dict_id = dict_id;
                    weight_array.weight_item[count].weight  = weight;
                    weight_array.count++;
                }

LOOP_NEXT:
                tmp_orig_list = tmp_orig_list->next;
            }

            /** 写倒排表 */
#if (_DEBUG)
            logprintf("[%s] hash_key: %lu, dict_id: %lu, weight:%f",
                task->prefix_array.data[i], hash_key, task->dict_id, weight);
#endif
            p = log_buf;
            p += snprintf(p, sizeof(log_buf) - (p - log_buf), "%s\t%lu\t",
              task->prefix_array.data[i], hash_key);

            qsort(weight_array.weight_item, weight_array.count, sizeof(weight_item_t), weight_cmp);
            for (k = 0; k < weight_array.count; k++)
            {
                p += snprintf(p, sizeof(log_buf) - (p - log_buf), "%lu,",
                    weight_array.weight_item[k].dict_id);

            }

            p += snprintf(p, sizeof(log_buf) - (p - log_buf), "\n");
            size = fwrite(log_buf, sizeof(char), p - log_buf, inverted_fp);
        } /** for every prefix */

        /** writ [index_dict] file */
        snprintf(log_buf, sizeof(log_buf), "%lu\t%s", task->dict_id, task->original_line);
        size = fwrite(log_buf, sizeof(char), strlen(log_buf), dict_fp);

#if (_DEBUG)
        fprintf(stderr, "  [%d] I handle task: %lu\n", tindex, task->dict_id);
#endif
        if (task)
        {
            free(task);
        }
     }

FATAL_ERROR:
    return NULL;
}

int main(int argc, char *argv[])
{
    int c;
    int i = 0;
    int ret = 0;
    int t_opt;
    char file_name[FILE_NAME_LEN];
    FILE *fp = NULL;

    size_t size = 0;
    /** 主线程,负责产生任务队列 */
    pthread_t  parse_core;
    /** 工作线程 */
    pthread_t *handle_core = NULL;
    /** 传递数据到工作线程 */
    argument_t *arg = NULL;

    signal_setup();
    init_config();

    while (-1 != (c = getopt(argc, argv,
        "s:"  /* max hash table size */
        "T:"  /* thread number */
        "v"   /* show version */
        "l:"  /* log level */
        "o:"  /* input original file name */
        "i:"  /* input inverted index file name */
        "x:"  /* input index dict file name */
        "h"  /* show usage */
    )))
    {
        switch (c)
        {
        case 's':
            size = (size_t)atoll(optarg);
            if (size > 0 && size > MAX_HASH_TABLE_SIZE)
            {
                gconfig.max_hash_table_size = size;
            }
            break;

        case 'T':
            size = (size_t)atoll(optarg);
            if (size > 0 && size > THREAD_NUM)
            {
                g_thread_num = (int)size;
            }
            break;

        case 'l':
            t_opt = atoi(optarg);
            if (t_opt >= 0)
            {
                gconfig.log_level = t_opt;
            }
            break;

        case 'o':   /** original file */
            if (strlen(optarg))
            {
                snprintf(g_original_file, FILE_NAME_LEN,
                    "%s", optarg);
            }
            break;

        case 'i':   /** inverted index */
            if (strlen(optarg))
            {
                snprintf(gconfig.inverted_index, FILE_NAME_LEN,
                    "%s", optarg);
            }
            break;

        case 'x':   /** index dict */
            if (strlen(optarg))
            {
                snprintf(gconfig.index_dict, FILE_NAME_LEN,
                    "%s", optarg);
            }
            break;

        case 'v':
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        }
    }

#if (_DEBUG)
    logprintf("original file: [%s]", g_original_file);
    logprintf("inverted index file: [%s]", gconfig.inverted_index);
    logprintf("index dict file: [%s]", gconfig.index_dict);
    logprintf("hash_table_size: %lu", gconfig.max_hash_table_size);
#endif

    /** 初始化全局结构体 */
    if (0 != init_hash_table())
    {
        fprintf(stderr, "init_hash_table fail.\n");
        exit(-11);
    }
#if (_DEBUG)
    else
    {
        logprintf("init hash table success.");
    }
#endif

    if (0 != load_original())
    {
        fprintf(stderr, "load original fail.\n");
        exit(-12);
    }
#if (_DEBUG)
    else
    {
        logprintf("load original data success.");
    }
#endif
    /** 申请输出文件针指内存空间 */
    size = sizeof(output_fp_t) * g_thread_num;
    output_fp = (output_fp_t *)malloc(size);
    if (NULL == output_fp)
    {
        fprintf(stderr, "Can NOT malloc memory for output_fp, need size: %lu\n", size);
        exit(-1);
    }

    /** 初始化任务队列互斥锁 */
    pthread_mutex_init(&task_queue_mutex, NULL);

    /** 初始化线程参数 */
    size = sizeof(argument_t) * g_thread_num;
    arg = (argument_t *)malloc(size);
    if (NULL == arg)
    {
        fprintf(stderr, "Can NOT malloc memory for arg, need size: %lu\n", size);
        exit(-1);
    }

    /* 分配工作者线程空间 */
    size = sizeof(pthread_t) * g_thread_num;
    handle_core = (pthread_t *)malloc(size);
    fprintf(stderr, "malloc threadid for work threads, thread num is %d\n", g_thread_num);
    if (NULL == handle_core)
    {
        fprintf(stderr, "Can NOT malloc memory for handle_core, need size: %lu\n", size);
        exit(-1);
    }

    ret = pthread_create(&(parse_core), NULL, parse_task, NULL);
    if ( 0 != ret )
    {
        fprintf(stderr, "Create parse_task thread fail.\n");
        exit (-1);
    }
#if (_DEBUG)
    fprintf(stderr, "create parse_task main thread OK.\n");
#endif

    /** give parse_task time to create task queue */
    usleep((useconds_t)(50));

    /** open fp for every worker */
    for (i = 0; i < g_thread_num; ++i)
    {
        snprintf(file_name, FILE_NAME_LEN, "%s.%d", gconfig.inverted_index, i);
        fp = fopen(file_name, "w");
        if (NULL == fp)
        {
            fprintf(stderr, "Can NOT open inverted file to write data: [%s]\n",
                file_name);
            exit(-11);
        }
        output_fp[i].inverted_fp = fp;

        snprintf(file_name, FILE_NAME_LEN, "%s.%d", gconfig.index_dict, i);
        fp = fopen(file_name, "w");
        if (NULL == fp)
        {
            fprintf(stderr, "Can NOT open inverted file to write data: [%s]\n",
                file_name);
            exit(-11);
        }
        output_fp[i].dict_fp = fp;
    }

    /** 创建每个工作者 */
    for (i = 0; i < g_thread_num; i++)
    {
        arg[i].tindex = i;
        ret = pthread_create(&handle_core[i], NULL, handle_task, (void *)&(arg[i]));
        if ( 0 != ret )
        {
            fprintf(stderr, "create the %d  handle_task thread fail.\n", i);
            exit (-1);
        }
#if (_DEBUG)
        fprintf(stderr, "create the %d the handle_task thread success, thread id is %lu.\n",
            i, (unsigned long int)handle_core[i]);
#endif
    }

    pthread_join(parse_core, NULL);

    for (i = 0; i < g_thread_num; ++i)
    {
        pthread_join(handle_core[i], NULL);

    }

    /** close all output fp */
    for (i = 0; i < g_thread_num; i++)
    {
        fclose(output_fp[i].inverted_fp);
        fclose(output_fp[i].dict_fp);
    }

    return 0;
}

