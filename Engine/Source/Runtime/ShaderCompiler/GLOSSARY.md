# ShaderCompiler

Shader compilation pipeline with a backend-per-language architecture: a facade dispatches pipeline compile requests to per-language backends, backend selection is explicit in each shader entry (file extensions are warning-only), and Slang is the current backend.

## Language

**ShaderCompiler**:
Public facade and entry point for pipeline shader compilation; routes each request to the correct backend.

**ShaderEntry**:
Request for one shader entry point in a source file; pipeline compile descriptors compose entries.

**GraphicsCompileDesc**:
Compile request for one graphics pipeline shader combination — composes and links the vertex and fragment entries in one request so the reflection describes the final pipeline interface.
