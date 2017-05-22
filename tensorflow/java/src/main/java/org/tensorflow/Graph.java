/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

package org.tensorflow;

/**
 * A data flow graph representing a TensorFlow computation.
 *
 * <p>Instances of a Graph are thread-safe.
 *
 * <p><b>WARNING:</b> Resources consumed by the Graph object must be explicitly freed by invoking
 * the {@link #close()} method then the Graph object is no longer needed.
 */
public final class Graph implements AutoCloseable {

  /** Create an empty Graph. */
  public Graph() {
    nativeHandle = allocate();
  }

  /** Create a Graph from an existing handle (takes ownership). */
  Graph(long nativeHandle) {
    this.nativeHandle = nativeHandle;
  }

  /**
   * Release resources associated with the Graph.
   *
   * <p>Blocks until there are no active {@link Session} instances referring to this Graph. A Graph
   * is not usable after close returns.
   */
  @Override
  public void close() {
    synchronized (nativeHandleLock) {
      if (nativeHandle == 0) {
        return;
      }
      while (refcount > 0) {
        try {
          nativeHandleLock.wait();
        } catch (InterruptedException e) {
          Thread.currentThread().interrupt();
          // Possible leak of the graph in this case?
          return;
        }
      }
      delete(nativeHandle);
      nativeHandle = 0;
    }
  }

  /**
   * Returns the operation (node in the Graph) with the provided name.
   *
   * <p>Or {@code null} if no such operation exists in the Graph.
   */
  public Operation operation(String name) {
    synchronized (nativeHandleLock) {
      long oph = operation(nativeHandle, name);
      if (oph == 0) {
        return null;
      }
      return new Operation(this, oph);
    }
  }

  /**
   * Returns a builder to add {@link Operation}s to the Graph.
   *
   * @param type of the Operation (i.e., identifies the computation to be performed)
   * @param name to refer to the created Operation in the graph.
   * @return an {@link OperationBuilder}, which will add the Operation to the graph when {@link
   *     OperationBuilder#build()} is invoked. If {@link OperationBuilder#build()} is not invoked,
   *     then some resources may leak.
   */
  public OperationBuilder opBuilder(String type, String name) {
    return new OperationBuilder(this, type, name);
  }

  /**
   * Import a serialized representation of a TensorFlow graph.
   *
   * <p>The serialized representation of the graph, often referred to as a <i>GraphDef</i>, can be
   * generated by {@link #toGraphDef()} and equivalents in other language APIs.
   *
   * @throws IllegalArgumentException if graphDef is not a recognized serialization of a graph.
   * @see #importGraphDef(byte[], String)
   */
  public void importGraphDef(byte[] graphDef) throws IllegalArgumentException {
    importGraphDef(graphDef, "");
  }

  /**
   * Import a serialized representation of a TensorFlow graph.
   *
   * @param graphDef the serialized representation of a TensorFlow graph.
   * @param prefix a prefix that will be prepended to names in graphDef
   * @throws IllegalArgumentException if graphDef is not a recognized serialization of a graph.
   * @see #importGraphDef(byte[])
   */
  public void importGraphDef(byte[] graphDef, String prefix) throws IllegalArgumentException {
    if (graphDef == null || prefix == null) {
      throw new IllegalArgumentException("graphDef and prefix cannot be null");
    }
    synchronized (nativeHandleLock) {
      importGraphDef(nativeHandle, graphDef, prefix);
    }
  }

  /**
   * Generate a serialized representation of the Graph.
   *
   * @see #importGraphDef(byte[])
   * @see #importGraphDef(byte[], String)
   */
  public byte[] toGraphDef() {
    synchronized (nativeHandleLock) {
      return toGraphDef(nativeHandle);
    }
  }

  private final Object nativeHandleLock = new Object();
  private long nativeHandle;
  private int refcount = 0;

  // Related native objects (such as the TF_Operation object backing an Operation instance)
  // have a validity tied to that of the Graph. The handles to those native objects are not
  // valid after Graph.close() has been invoked.
  //
  // Instances of the Reference class should be used to ensure the Graph has not been closed
  // while dependent handles are in use.
  class Reference implements AutoCloseable {
    private Reference() {
      synchronized (Graph.this.nativeHandleLock) {
        active = Graph.this.nativeHandle != 0;
        if (!active) {
          throw new IllegalStateException("close() has been called on the Graph");
        }
        active = true;
        Graph.this.refcount++;
      }
    }

    @Override
    public void close() {
      synchronized (Graph.this.nativeHandleLock) {
        if (!active) {
          return;
        }
        active = false;
        if (--Graph.this.refcount == 0) {
          Graph.this.nativeHandleLock.notifyAll();
        }
      }
    }

    public long nativeHandle() {
      synchronized (Graph.this.nativeHandleLock) {
        return active ? Graph.this.nativeHandle : 0;
      }
    }

    private boolean active;
  }

  Reference ref() {
    return new Reference();
  }

  private static native long allocate();

  private static native void delete(long handle);

  private static native long operation(long handle, String name);

  private static native void importGraphDef(long handle, byte[] graphDef, String prefix)
      throws IllegalArgumentException;

  private static native byte[] toGraphDef(long handle);

  static {
    TensorFlow.init();
  }
}
