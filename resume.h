#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "download.h"

// Сохранение прогресса: какие куски раздачи уже скачаны и проверены.
// Благодаря этому после перезапуска приложения не нужно заново считать все файлы.

// Папка для файлов прогресса (по умолчанию /data/ps4torrent/resume).
void resume_set_dir(const std::string& dir);

bool resume_save(const std::string& hash, uint32_t numPieces,
                 const std::vector<bool>& have, const std::vector<uint32_t>& recent);

// true, если файл есть, целый и относится к раздаче с таким числом кусков.
bool resume_load(const std::string& hash, uint32_t numPieces,
                 std::vector<bool>& have, std::vector<uint32_t>& recent);

void resume_remove(const std::string& hash);     // удаляет и файл с недокачанными кусками

// Недокачанные куски (какие блоки уже записаны). Пустой список = файл удаляется.
bool resume_save_partial(const std::string& hash, uint32_t numPieces, const std::vector<PartialPiece>& parts);
// true, если файл есть и целый; parts пуст, если сохранять было нечего.
bool resume_load_partial(const std::string& hash, uint32_t numPieces, std::vector<PartialPiece>& parts);

// Варианты с явной папкой (не трогают общее состояние, можно вызывать из потока записи).
bool resume_save_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<bool>& have,
                    const std::vector<uint32_t>& recent);
bool resume_merge_save_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<bool>& have,
                          const std::vector<uint32_t>& recent);     // объединяет с уже сохранённым
// Выбор файлов раздачи: список снятых файлов вида "3,5-9" (пусто = выбраны все; файл тогда удаляется).
bool resume_save_sel_at(const std::string& dir, const std::string& hash, const std::string& skipList);
bool resume_load_sel(const std::string& dir, const std::string& hash, std::string& skipList);
bool resume_save_partial_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<PartialPiece>& parts);
