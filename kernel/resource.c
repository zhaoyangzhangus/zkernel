#include <kernel/kmem.h>
#include <kernel/process.h>
#include <kernel/resource.h>

static void resource_lock(spinlock_t *lock) {
    spinlock_lock(lock);
}

static void resource_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
}

static void job_destroy(void *object) {
    job_t *job = (job_t *)object;
    /* 每个挂接进程都持有 Job 引用，最后一个引用释放时列表必为空。 */
    if (job->parent != 0) object_put(job->parent);
    kfree(job);
}

static const object_ops_t g_job_ops = {
    .destroy = job_destroy,
    .type_name = "Job",
    .is_signaled = 0,
    .wait_value = 0,
};

kstatus_t job_create(job_t *parent, const job_limits_t *limits, job_t **out) {
    if (out == 0) return K_EINVAL;
    job_t *job = (job_t *)kzalloc(sizeof(*job), 0);
    if (job == 0) return K_ENOMEM;
    object_header_init(&job->object, KOBJECT_TYPE_JOB, &g_job_ops);
    spinlock_init(&job->lock);
    list_init(&job->processes);
    job->parent = parent;
    if (parent != 0) object_get(parent);
    if (limits != 0) job->limits = *limits;
    atomic_init(&job->committed_bytes, 0U);
    atomic_init(&job->cpu_time_ns, 0U);
    *out = job;
    return K_OK;
}

kstatus_t job_attach_process(job_t *job, struct process *process) {
    if (job == 0 || process == 0) return K_EINVAL;
    resource_lock(&job->lock);
    if (process->job != 0 || process->job_node.next != &process->job_node) {
        resource_unlock(&job->lock);
        return K_EBUSY;
    }
    object_get(job);
    process->job = job;
    list_add_tail(&job->processes, &process->job_node);
    resource_unlock(&job->lock);
    return K_OK;
}

kstatus_t job_detach_process(struct process *process) {
    if (process == 0) return K_EINVAL;
    job_t *job = process->job;
    if (job == 0) return K_ENOENT;
    bool detached = false;
    resource_lock(&job->lock);
    if (process->job == job && process->job_node.next != &process->job_node) {
        list_del(&process->job_node);
        process->job = 0;
        detached = true;
    }
    resource_unlock(&job->lock);
    /* 只有移除该关联的操作才拥有对应引用。 */
    if (detached) {
        object_put(job);
        return K_OK;
    }
    return K_EBUSY;
}

bool resource_core_self_test(void) {
    job_t *root = 0;
    job_t *child = 0;
    bool success = job_create(0, 0, &root) == K_OK &&
                   job_create(root, 0, &child) == K_OK &&
                   root != 0 && child != 0 &&
                   root->object.type == KOBJECT_TYPE_JOB &&
                   child->parent == root &&
                   child->object.type == KOBJECT_TYPE_JOB;
    if (child != 0) object_put(child);
    if (root != 0) object_put(root);
    return success;
}
