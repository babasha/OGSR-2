// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — SECONDARY COMMAND BUFFERS, per lane, per frame-in-flight.
//
// ⭐ЗАЧЕМ. Запись командных буферов — 1.0-1.4 мс, крупнейшая одиночная статья
// CPU-кадра, и она ПОСЛЕДОВАТЕЛЬНА: пятнадцать пассов пишут в один primary,
// потому что VkCommandBuffer нельзя писать из двух потоков. Это единственное
// место кадра, где возможна КРАТНОСТЬ, а не проценты: пассы между собой почти
// независимы по CPU-работе, и пишутся они в ОДИН буфер только потому, что так
// сложилось. Вторичные буферы снимают это ограничение: каждый пасс пишет в
// СВОЙ буфер, а primary потом проигрывает их по порядку через
// vkCmdExecuteCommands — порядок GPU сохраняется в точности.
//
// ⚠ЭТАП А (то, что здесь) ещё НЕ МНОГОПОТОЧЕН. Он вводит только сам механизм и
// проверяет, что ни один пасс не делает во вторичном буфере ничего
// незаконного, — запись по-прежнему идёт на главном потоке и по порядку.
// Разделять на потоки можно только после того, как это отработало с
// -vk_validation: диагностировать гонку и незаконную команду одновременно —
// это два эксперимента в одном, и оба выйдут неубедительными.
//
// ⚠VkCommandPool НЕ потокобезопасен — отсюда «дорожки» (lanes): у каждого
// потока записи свой пул на каждый кадр в полёте, и никакой блокировки на
// горячем пути не нужно. Пул сбрасывается целиком раз в кадр (TRANSIENT) —
// это дешевле, чем сбрасывать буферы по одному.
//
// ⚠Вторичный буфер, который САМ открывает vkCmdBeginRendering, не должен
// наследовать область рендеринга: inheritanceInfo.renderPass = NULL и БЕЗ
// флага RENDER_PASS_CONTINUE. Пассы этого движка каждый открывают свою область
// сам, поэтому годится именно такой режим, а vkCmdExecuteCommands зовётся
// СНАРУЖИ любой области рендеринга.

#pragma once
#include "vk_core.h"

namespace VK
{
namespace SecCB
{
    // Сколько дорожек записи держим. 1 = этап А (одна, главный поток).
    constexpr u32 kMaxLanes = 8;

    // Раз в кадр, ДО первого Acquire: сбрасывает пулы этого слота in-flight.
    // frameIndex — тот же, что у CommandManager.GetCurrentFrame().
    void BeginFrame(u32 frameIndex);

    // Открыть свежий вторичный буфер на дорожке lane и начать в него запись.
    // Возвращает VK_NULL_HANDLE, если механизм не поднялся (вызывающий обязан
    // тогда писать в primary — молчаливая потеря пасса недопустима).
    VkCommandBuffer Acquire(u32 lane);

    // Закрыть запись. Буфер остаётся валидным до следующего BeginFrame этого
    // слота in-flight — то есть ровно до момента, когда кадр уже отработал.
    void Finish(VkCommandBuffer cb);

    void Destroy();
}
}
