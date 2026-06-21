# UEPI Extension SDK

## Asset Adapter

Implement `UE::ProjectIntelligence::IAssetAdapter` when a project or plugin owns a custom asset class that needs more than L0/L1 metadata.

Minimum responsibilities:

- Return stable adapter id and version.
- Match assets using `FAssetData`.
- Append domain entities and relations.
- Add evidence strings that point back to the source object, property, graph, or asset path.
- Mark incomplete data in `Completeness`.

## Node Semantic Adapter

Implement `UE::ProjectIntelligence::INodeSemanticAdapter` for custom `UEdGraphNode` classes.

Minimum responsibilities:

- Preserve the canonical node/pin/link output.
- Add semantic relations only when the adapter can prove them.
- Mark derived relations as derived and attach evidence to source pins or node properties.
- Never compile Blueprints or execute node logic while extracting semantics.

## Registration Sketch

```cpp
IModularFeatures::Get().RegisterModularFeature(
    UE::ProjectIntelligence::IAssetAdapter::FeatureName(),
    &AdapterInstance);
```

Unregister the feature during module shutdown. Keep adapters deterministic: the same asset input should produce the same IDs and attributes.
