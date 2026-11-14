import Editor.SelectionFilter;

module SelectionIndexedCommandConsumer;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() {
    RasterIndirectCommand command;
    command.indexCount = 6;
    command.instanceCount = 2;
    command.firstIndex = 0;
    command.vertexOffset = 0;
    command.firstInstance = 4;
}
