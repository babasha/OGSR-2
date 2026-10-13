#pragma once

std::string BuildStackTrace(const char* header);
std::string BuildStackTrace(const char* header, PCONTEXT threadCtx);

// ⭐ИМЯ ФУНКЦИИ ПО АДРЕСУ ВОЗВРАТА. Нужен там, где полный стек снимать нельзя по
// цене (сотни тысяч вызовов в секунду), а знать ВЫЗЫВАЮЩУЮ сторону необходимо:
// прибор копит сырой `_ReturnAddress()` на горячем пути и разрешает его в имя
// один раз при печати отчёта. Возвращает "Fun + off (file:line)" либо сам адрес.
// XRCORE_API: нужен не только внутри xrCore — перепись костных колбэков в
// Layers/xrRender разрешает указатели на функции в имена ровно так же.
XRCORE_API std::string ResolveCodeAddress(const void* addr);
