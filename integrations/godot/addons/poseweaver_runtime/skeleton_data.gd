extends Resource
## Embedded document and imported resources survive game exports.
@export_multiline var document := ""
@export var images: Array[Texture2D] = []
@export var audio: Array[AudioStream] = []
