/**
 * worker入口文件
 */
#include "worker.h"

zend_class_entry workerman_worker_ce;
zend_class_entry *workerman_worker_ce_ptr;

//zend_object_handlers实际上就是我们在PHP脚本上面操作一个PHP对象的时候，底层会去调用的函数。
static zend_object_handlers workerman_worker_handlers;

/**
 * 通过这个PHP对象找到我们的wmConnectionObject对象的代码
 */
wmWorkerObject* wm_worker_fetch_object(zend_object *obj) {
	return (wmWorkerObject*) ((char*) obj - workerman_worker_handlers.offset);
}

/**
 * 创建一个php对象
 * zend_class_entry是一个php类
 */
static zend_object* wmWorker_create_object(zend_class_entry *ce) {
	wmWorkerObject *worker_obj = (wmWorkerObject*) ecalloc(1, sizeof(wmWorkerObject) + zend_object_properties_size(ce));
	zend_object_std_init(&worker_obj->std, ce);
	object_properties_init(&worker_obj->std, ce);
	worker_obj->std.handlers = &workerman_worker_handlers;
	return &worker_obj->std;
}

/**
 * 释放php对象
 * PS:针对Worker对象，程序是走不到这里的，都是通过exit(1)在合适的地方终止了，所以没有过多释放worker内存的操作
 */
static void wmWorker_free_object(zend_object *object) {
	wmWorkerObject *worker_obj = (wmWorkerObject*) wm_worker_fetch_object(object);
	//这里应该释放worker内存的，但是程序是走不到这里
	wmWorker_free(worker_obj->worker);
	zend_object_std_dtor(&worker_obj->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_workerman_worker_void, 0, 0, 0) //
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_workerman_worker_construct, 0, 0, 1) //
ZEND_ARG_INFO(0, socketName) //
ZEND_END_ARG_INFO()

PHP_METHOD(workerman_worker, __construct) {
	zend_string *socketName;

	ZEND_PARSE_PARAMETERS_START(1, 1)
				Z_PARAM_STR(socketName)
			ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);
	wmWorkerObject *worker_obj = (wmWorkerObject*) wm_worker_fetch_object(Z_OBJ_P(getThis()));
	zval *worker_zval = wm_malloc_zval();
	ZVAL_OBJ(worker_zval, &worker_obj->std);
	//初始化worker
	worker_obj->worker = wmWorker_create(worker_zval, socketName);

	//设置workerId
	zend_update_property_long(workerman_worker_ce_ptr, getThis(), ZEND_STRL("workerId"), worker_obj->worker->workerId);
}

/**
 * 全部运行
 */
PHP_METHOD(workerman_worker, runAll) {
	//检查环境
	wmWorker_runAll();
	RETURN_TRUE
}

/**
 * 关闭
 */
PHP_METHOD(workerman_worker, stopAll) {
	//检查环境
	wmWorker_stopAll();
}

/**
 * 实例化Worker后执行监听，用于在Worker进程内创建Worker
 */
PHP_METHOD(workerman_worker, listen) {
	wmWorkerObject *worker_obj;
	worker_obj = (wmWorkerObject*) wm_worker_fetch_object(Z_OBJ_P(getThis()));
	//创建一个新协程
	zend_fcall_info_cache _listen;
	wm_get_internal_function(worker_obj->worker->_This, workerman_worker_ce_ptr, ZEND_STRL("_listen"), &_listen);
	wmCoroutine_create(&_listen, 0, NULL);
}

/**
 * 平滑重启Worker
 */
PHP_METHOD(workerman_worker, reload) {
	wmWorkerObject *worker_obj;
	worker_obj = (wmWorkerObject*) wm_worker_fetch_object(Z_OBJ_P(getThis()));
	wmWorker_reload(worker_obj->worker);
}

/**
 * 获取 Worker 当前请求数量
 */
PHP_METHOD(workerman_worker, requestNum) {
    long num = wmWorker_requestNum();
    RETURN_LONG(num);
}

/**
 *  私有方法，扩展用
 */
PHP_METHOD(workerman_worker, _listen) {
	wmWorkerObject *worker_obj;
	worker_obj = (wmWorkerObject*) wm_worker_fetch_object(Z_OBJ_P(getThis()));
	wmWorker_listen(worker_obj->worker);
	wmWorker_resumeAccept(worker_obj->worker);
}

/**
 * 给worker内部使用
 */
PHP_METHOD(workerman_worker, run) {
	wmWorkerObject *worker_obj;
	worker_obj = (wmWorkerObject*) wm_worker_fetch_object(Z_OBJ_P(getThis()));
	wmWorker_run(worker_obj->worker);
}

/**
 * 命名空间改为Workerman
 */
PHP_METHOD(workerman_worker, rename) {
	//将Warriorman修改成Workerman
	zend_register_ns_class_alias("Workerman", "Worker", workerman_worker_ce_ptr);
	zend_register_ns_class_alias("Workerman", "Lib\\Timer", workerman_timer_ce_ptr);
	zend_register_ns_class_alias("Workerman", "Runtime", workerman_runtime_ce_ptr);
	zend_register_ns_class_alias("Workerman", "Coroutine", workerman_coroutine_ce_ptr);
	zend_register_ns_class_alias("Workerman", "Connection\\TcpConnection", workerman_connection_ce_ptr);
	zend_register_ns_class_alias("Workerman", "Channel", workerman_channel_ce_ptr);
}

static const zend_function_entry workerman_worker_methods[] = { //
	PHP_ME(workerman_worker, __construct, arginfo_workerman_worker_construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR) // ZEND_ACC_CTOR is used to declare that this method is a constructor of this class.
		PHP_ME(workerman_worker, runAll, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
		PHP_ME(workerman_worker, stopAll, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC| ZEND_ACC_STATIC)
		PHP_ME(workerman_worker, _listen, arginfo_workerman_worker_void, ZEND_ACC_PRIVATE)
		PHP_ME(workerman_worker, listen, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC)
		PHP_ME(workerman_worker, reload, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC)
		PHP_ME(workerman_worker, requestNum, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC)
		PHP_ME(workerman_worker, run, arginfo_workerman_worker_void, ZEND_ACC_PRIVATE)
		PHP_ME(workerman_worker, rename, arginfo_workerman_worker_void, ZEND_ACC_PUBLIC| ZEND_ACC_STATIC)
		PHP_FE_END };

/**
 * 注册我们的WorkerMan\Server这个类
 */
void workerman_worker_init() {
	//定义好一个类
	INIT_NS_CLASS_ENTRY(workerman_worker_ce, "Warriorman", "Worker", workerman_worker_methods);
	//在zedn中注册类
	workerman_worker_ce_ptr = zend_register_internal_class(&workerman_worker_ce TSRMLS_CC); // 在 Zend Engine 中注册

	//替换掉PHP默认的handler
	memcpy(&workerman_worker_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	//php对象实例化已经由我们自己的代码接管了
	workerman_worker_ce_ptr->create_object = wmWorker_create_object;
	workerman_worker_handlers.free_obj = wmWorker_free_object;
	workerman_worker_handlers.offset = (zend_long) (((char*) (&(((wmWorkerObject*) NULL)->std))) - ((char*) NULL));

	//注册变量和初始值
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onWorkerStart"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onWorkerStop"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onWorkerReload"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onConnect"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onMessage"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onClose"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onBufferFull"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onBufferDrain"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("onError"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("name"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("user"), ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("protocol"), ZEND_ACC_PUBLIC);
	zend_declare_property_long(workerman_worker_ce_ptr, ZEND_STRL("count"), 1, ZEND_ACC_PUBLIC);
	zend_declare_property_long(workerman_worker_ce_ptr, ZEND_STRL("workerId"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("connections"), ZEND_ACC_PUBLIC);
	zend_declare_property_bool(workerman_worker_ce_ptr, ZEND_STRL("reusePort"), 1, ZEND_ACC_PUBLIC);
	zend_declare_property_long(workerman_worker_ce_ptr, ZEND_STRL("backlog"), WM_DEFAULT_BACKLOG, ZEND_ACC_PUBLIC);

	//静态变量
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("pidFile"), ZEND_ACC_PUBLIC | ZEND_ACC_STATIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("logFile"), ZEND_ACC_PUBLIC | ZEND_ACC_STATIC);
	zend_declare_property_null(workerman_worker_ce_ptr, ZEND_STRL("stdoutFile"), ZEND_ACC_PUBLIC | ZEND_ACC_STATIC);
	zend_declare_property_bool(workerman_worker_ce_ptr, ZEND_STRL("daemonize"), 0, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC);

	//常量
	zval VERSION;
	ZVAL_NEW_STR(&VERSION, zend_string_init(ZEND_STRL(PHP_WORKERMAN_VERSION), 1));
	zend_declare_class_constant(workerman_worker_ce_ptr, ZEND_STRL("VERSION"), &VERSION);
}
