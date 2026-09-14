/**
 * @file    ai_kit_ml_rtos.c
 * @brief   Infineon ML 中间件（mtb_ml）的 RTOS 适配层
 *
 * @details 当 CY_RTOS_AWARE 被使能时（Wi-Fi 栈通过 abstraction-rtos 间接
 *          打开），mtb_ml 中间件需要外部提供互斥量与信号量的实现。
 *          本文件实现 mtb_ml_mutex_* 与 mtb_ml_sem_* 系列弱符号，把调用
 *          转发到 cyabs_rtos（cy_rtos_*）以接入 RT-Thread 调度器。
 *
 *          所有对象用 malloc 分配，destroy 时 free。timeout 转换函数
 *          把 uint64_t ms 折算为 cyabs_rtos 的 uint32_t 上限。
 */
#include <stdint.h>
#include <stdlib.h>

#include "cyabs_rtos.h"

/**
 * @brief   把 mtb_ml 的 uint64_t 超时值转换为 cyabs_rtos 的 uint32_t
 *
 * @details mtb_ml 用 UINT64_MAX 表示"永不超时"，cyabs_rtos 用
 *          CY_RTOS_NEVER_TIMEOUT 表示；本函数统一映射。其他值若超过
 *          uint32_t 上限则截断为 CY_RTOS_NEVER_TIMEOUT-1，避免被误判
 *          为"永不超时"。
 *
 * @param   timeout  mtb_ml 侧的超时（ms），UINT64_MAX=永不超时
 *
 * @return  cyabs_rtos 侧的 uint32_t 超时
 */
static uint32_t ai_kit_ml_timeout_ms(uint64_t timeout)
{
    if (timeout == UINT64_MAX)
    {
        return CY_RTOS_NEVER_TIMEOUT;
    }

    return (timeout >= CY_RTOS_NEVER_TIMEOUT) ?
           (CY_RTOS_NEVER_TIMEOUT - 1U) : (uint32_t)timeout;
}

/**
 * @brief   创建一个互斥量（mtb_ml 接口）
 *
 * @details malloc 一块 cy_mutex_t，调用 cy_rtos_init_mutex 初始化。
 *          失败时 free 并返回 NULL。
 *
 * @return  互斥量句柄；失败返回 NULL
 */
void *mtb_ml_mutex_create(void)
{
    cy_mutex_t *mutex = (cy_mutex_t *)malloc(sizeof(*mutex));

    if ((mutex == NULL) || (cy_rtos_init_mutex(mutex) != CY_RSLT_SUCCESS))
    {
        free(mutex);
        return NULL;
    }

    return mutex;
}

/**
 * @brief   获取互斥量（mtb_ml 接口，永久等待）
 *
 * @param   mutex  mtb_ml_mutex_create 返回的句柄
 *
 * @retval  0   成功
 * @retval  -1  mutex 为空或获取失败
 */
int mtb_ml_mutex_lock(void *mutex)
{
    return (mutex != NULL) &&
           (cy_rtos_get_mutex((cy_mutex_t *)mutex,
                              CY_RTOS_NEVER_TIMEOUT) == CY_RSLT_SUCCESS) ? 0 : -1;
}

/**
 * @brief   释放互斥量（mtb_ml 接口）
 *
 * @param   mutex  mtb_ml_mutex_create 返回的句柄
 *
 * @retval  0   成功
 * @retval  -1  mutex 为空或释放失败
 */
int mtb_ml_mutex_unlock(void *mutex)
{
    return (mutex != NULL) &&
           (cy_rtos_set_mutex((cy_mutex_t *)mutex) == CY_RSLT_SUCCESS) ? 0 : -1;
}

/**
 * @brief   销毁互斥量（mtb_ml 接口）
 *
 * @details 反初始化 cy_mutex_t 并 free 内存；mutex 为 NULL 时无动作。
 *
 * @param   mutex  mtb_ml_mutex_create 返回的句柄
 */
void mtb_ml_mutex_destroy(void *mutex)
{
    if (mutex != NULL)
    {
        (void)cy_rtos_deinit_mutex((cy_mutex_t *)mutex);
        free(mutex);
    }
}

/**
 * @brief   创建一个信号量（mtb_ml 接口）
 *
 * @details 初始 max=1，初始 count=0（典型的二值信号量）。
 *
 * @return  信号量句柄；失败返回 NULL
 */
void *mtb_ml_sem_create(void)
{
    cy_semaphore_t *sem = (cy_semaphore_t *)malloc(sizeof(*sem));

    if ((sem == NULL) ||
        (cy_rtos_init_semaphore(sem, 1U, 0U) != CY_RSLT_SUCCESS))
    {
        free(sem);
        return NULL;
    }

    return sem;
}

/**
 * @brief   等待信号量（mtb_ml 接口）
 *
 * @param   sem      mtb_ml_sem_create 返回的句柄
 * @param   timeout  超时（ms），UINT64_MAX=永不超时
 *
 * @retval  0   成功
 * @retval  -1  sem 为空或等待超时 / 失败
 */
int mtb_ml_sem_take(void *sem, uint64_t timeout)
{
    return (sem != NULL) &&
           (cy_rtos_get_semaphore((cy_semaphore_t *)sem,
                                  ai_kit_ml_timeout_ms(timeout),
                                  false) == CY_RSLT_SUCCESS) ? 0 : -1;
}

/**
 * @brief   销毁信号量（mtb_ml 接口）
 *
 * @details 反初始化 cy_semaphore_t 并 free 内存；sem 为 NULL 时无动作。
 *
 * @param   sem  mtb_ml_sem_create 返回的句柄
 */
void mtb_ml_sem_destroy(void *sem)
{
    if (sem != NULL)
    {
        (void)cy_rtos_deinit_semaphore((cy_semaphore_t *)sem);
        free(sem);
    }
}
