import Foundation
import SceneKit
import SpriteKit
import GameplayKit
import AppKit
let scene = SCNScene()
let box = SCNNode(geometry: SCNBox(width: 1, height: 1, length: 1, chamferRadius: 0))
box.position = SCNVector3(1, 2, 3); scene.rootNode.addChildNode(box)
let cam = SCNNode(); cam.camera = SCNCamera(); cam.position = SCNVector3(0, 0, 10); scene.rootNode.addChildNode(cam)
print("scenekit", scene.rootNode.childNodes.count, box.boundingBox.max.x, box.convertPosition(SCNVector3Zero, to: nil).y)
let proj = SCNMatrix4MakeRotation(.pi / 2, 0, 0, 1); print("matrix", Int(proj.m11.rounded()), Int(proj.m12.rounded()))
let sk = SKScene(size: CGSize(width: 100, height: 100)); sk.physicsWorld.gravity = CGVector(dx: 0, dy: -10)
let node = SKSpriteNode(color: .red, size: CGSize(width: 10, height: 10)); node.position = CGPoint(x: 50, y: 90); node.physicsBody = SKPhysicsBody(rectangleOf: node.size)
sk.addChild(node)
let label = SKLabelNode(text: "hi"); sk.addChild(label)
print("spritekit", sk.children.count, node.frame.width, sk.calculateAccumulatedFrame().width > 0)
node.run(SKAction.moveBy(x: 10, y: 0, duration: 1)); print("actions", node.hasActions())
let tex = SKTexture(image: NSImage(size: NSSize(width: 8, height: 8), flipped: false) { r in NSColor.green.setFill(); r.fill(); return true }); print("texture", tex.size())
let rng = GKMersenneTwisterRandomSource(seed: 42); print("gk random", rng.nextInt(upperBound: 100), rng.nextInt(upperBound: 100))
let g = GKGraph(); let n1 = GKGraphNode2D(point: vector_float2(0, 0)), n2 = GKGraphNode2D(point: vector_float2(1, 0)), n3 = GKGraphNode2D(point: vector_float2(2, 0))
g.add([n1, n2, n3]); n1.addConnections(to: [n2], bidirectional: true); n2.addConnections(to: [n3], bidirectional: true)
print("gk path", g.findPath(from: n1, to: n3).count)
let sm = GKStateMachine(states: [GKState(), GKState()]); print("gk state", sm.enter(GKState.self))
let tree = GKRTree<NSString>(maxNumberOfChildren: 4); tree.addElement("a", boundingRectMin: vector_float2(0, 0), boundingRectMax: vector_float2(1, 1), splitStrategy: .linear)
print("gk rtree", tree.elements(inBoundingRectMin: vector_float2(0, 0), rectMax: vector_float2(2, 2)).count)
