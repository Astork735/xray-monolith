#pragma once
namespace vr
{
template<class Queue, class Frame> void RunFrameWork(Queue& parallel, Frame frame)
{
    for (unsigned task = 0; task < parallel.size(); ++task) parallel[task]();
    parallel.clear_not_free();
    frame();
}
}
