#pragma once
#include "../../comm/util.hpp"

// 对外读取数据，写入数据

// 实际是通过vecotr<char>中的size来管理读写的空间大小的，一个size一个字节
#define DEFAULT_BUF 1024
class Buffer
{
  private:
    std::vector<char> _buf;
    uint64_t _read_idx;
    uint64_t _write_idx;

  public:
    Buffer() : _buf(DEFAULT_BUF), _read_idx(0), _write_idx(0) {}
    char *BeginAddr()
    {
        return &*_buf.begin();
    } // 获得首地址
    char *ReadPositon()
    {
        return BeginAddr() + _read_idx;
    } // 读的地址
    char *WirtePositon()
    {
        return BeginAddr() + _write_idx;
    } // 写的地址
    void MoveReadIdx(uint64_t len)
    {
        _read_idx += len;
    } // 移动读指针索引
    void MoveWriteIdx(uint64_t len)
    {
        _write_idx += len;
    } // 移动写指针索引

    uint64_t AbleWriteSpace()
    {
        return _buf.size() - AbleReadSpace();
    } // 可写空间
    uint64_t AbleReadSpace()
    {
        return _write_idx - _read_idx;
    } // 可读空间

    void EnsureWriteSpace(uint64_t len)
    {
        // 如果尾部可写空间大于写长度，直接写
        if (_buf.size() - _write_idx >= len)
        {
            return;
        }
        // 如果可写空间小于要写长度，直接扩容len，减少拷贝
        if (AbleWriteSpace() < len)
        {
            _buf.resize(_write_idx + len);
            return;
        }
        // 如果尾部小于len，但可写空间大于len，则把已写的拷贝到最前面，减少空间支出
        if (_buf.size() - _write_idx < len)
        {
            uint64_t ret = AbleReadSpace();
            std::copy(ReadPositon(), WirtePositon(), BeginAddr());
            _read_idx = 0;    // 移动读指针
            _write_idx = ret; // 写指针为可读空间的大小
            return;
        }
    }

    void Write(const void *data, uint64_t len)
    {
        EnsureWriteSpace(len);
        std::copy((char *)data, (char *)data + len,
                  WirtePositon()); // 缓冲区不负责直接系统调用write、read，只负责储存，copy就行
        MoveWriteIdx(len);
    }
    void Write(Buffer& data)
    {
        Write(data.ReadPositon(), data.AbleReadSpace());
    }
    void Read(void *buffer, uint64_t len)
    {
        assert(AbleReadSpace() >= len);
        std::copy(ReadPositon(), ReadPositon() + len, (char*)buffer);
        MoveReadIdx(len);
    }
    ~Buffer() {}
};
